
/*++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
                            main.c
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
                                                    Xiao hong, 2016
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*/

#include "type.h"
#include "stdio.h"
#include "const.h"
#include "protect.h"
#include "string.h"
#include "fs.h"
#include "proc.h"
#include "tty.h"
#include "console.h"d
#include "global.h"
#include "proto.h"


/*****************************************************************************
 *                               kernel_main
 *****************************************************************************/
/**
 * jmp from kernel.asm::_start. 
 * 
 *****************************************************************************/
char location[128] = "/";
char users[2][128] = {"sjw", "empty"};
char curUser[128]="sjw";
char curFile[128]="Usr1";
char fileNames[20][128]={"","Usr1"};

int FAT[5][5]={0};
int fileCount=2;
char passwords[2][128]={"123","456"};





PUBLIC int kernel_main()
{
	disp_str("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n"
		 "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");

	int i, j, eflags, prio;
        u8  rpl;
        u8  priv; /* privilege */

	struct task * t;
	struct proc * p = proc_table;

	char * stk = task_stack + STACK_SIZE_TOTAL;

	for (i = 0; i < NR_TASKS + NR_PROCS; i++,p++,t++) {
		if (i >= NR_TASKS + NR_NATIVE_PROCS) {
			p->p_flags = FREE_SLOT;
			continue;
		}

	        if (i < NR_TASKS) {     /* TASK */
                        t	= task_table + i;
                        priv	= PRIVILEGE_TASK;
                        rpl     = RPL_TASK;
                        eflags  = 0x1202;/* IF=1, IOPL=1, bit 2 is always 1 */
			prio    = 15;
                }
                else {                  /* USER PROC */
                        t	= user_proc_table + (i - NR_TASKS);
                        priv	= PRIVILEGE_USER;
                        rpl     = RPL_USER;
                        eflags  = 0x202;	/* IF=1, bit 2 is always 1 */
			prio    = 5;
                }

		strcpy(p->name, t->name);	/* name of the process */
		p->p_parent = NO_TASK;

		if (strcmp(t->name, "INIT") != 0) {
			p->ldts[INDEX_LDT_C]  = gdt[SELECTOR_KERNEL_CS >> 3];
			p->ldts[INDEX_LDT_RW] = gdt[SELECTOR_KERNEL_DS >> 3];

			/* change the DPLs */
			p->ldts[INDEX_LDT_C].attr1  = DA_C   | priv << 5;
			p->ldts[INDEX_LDT_RW].attr1 = DA_DRW | priv << 5;
		}
		else {		/* INIT process */
			unsigned int k_base;
			unsigned int k_limit;
			int ret = get_kernel_map(&k_base, &k_limit);
			assert(ret == 0);
			init_desc(&p->ldts[INDEX_LDT_C],
				  0, /* bytes before the entry point
				      * are useless (wasted) for the
				      * INIT process, doesn't matter
				      */
				  (k_base + k_limit) >> LIMIT_4K_SHIFT,
				  DA_32 | DA_LIMIT_4K | DA_C | priv << 5);

			init_desc(&p->ldts[INDEX_LDT_RW],
				  0, /* bytes before the entry point
				      * are useless (wasted) for the
				      * INIT process, doesn't matter
				      */
				  (k_base + k_limit) >> LIMIT_4K_SHIFT,
				  DA_32 | DA_LIMIT_4K | DA_DRW | priv << 5);
		}

		p->regs.cs = INDEX_LDT_C << 3 |	SA_TIL | rpl;
		p->regs.ds =
			p->regs.es =
			p->regs.fs =
			p->regs.ss = INDEX_LDT_RW << 3 | SA_TIL | rpl;
		p->regs.gs = (SELECTOR_KERNEL_GS & SA_RPL_MASK) | rpl;
		p->regs.eip	= (u32)t->initial_eip;
		p->regs.esp	= (u32)stk;
		p->regs.eflags	= eflags;

		p->ticks = p->priority = prio;

		p->p_flags = 0;
		p->p_msg = 0;
		p->p_recvfrom = NO_TASK;
		p->p_sendto = NO_TASK;
		p->has_int_msg = 0;
		p->q_sending = 0;
		p->next_sending = 0;

		for (j = 0; j < NR_FILES; j++)
			p->filp[j] = 0;

		stk -= t->stacksize;
	}

	k_reenter = 0;
	ticks = 0;

	p_proc_ready	= proc_table;

	init_clock();
        init_keyboard();

	restart();

	while(1){}
}


/*****************************************************************************
 *                                get_ticks
 *****************************************************************************/
PUBLIC int get_ticks()
{
	MESSAGE msg;
	reset_msg(&msg);
	msg.type = GET_TICKS;
	send_recv(BOTH, TASK_SYS, &msg);
	return msg.RETVAL;
}


/**
 * @struct posix_tar_header
 * Borrowed from GNU `tar'
 */
struct posix_tar_header
{				/* byte offset */
	char name[100];		/*   0 */
	char mode[8];		/* 100 */
	char uid[8];		/* 108 */
	char gid[8];		/* 116 */
	char size[12];		/* 124 */
	char mtime[12];		/* 136 */
	char chksum[8];		/* 148 */
	char typeflag;		/* 156 */
	char linkname[100];	/* 157 */
	char magic[6];		/* 257 */
	char version[2];	/* 263 */
	char uname[32];		/* 265 */
	char gname[32];		/* 297 */
	char devmajor[8];	/* 329 */
	char devminor[8];	/* 337 */
	char prefix[155];	/* 345 */
	/* 500 */
};

/*****************************************************************************
 *                                untar
 *****************************************************************************/
/**
 * Extract the tar file and store them.
 * 
 * @param filename The tar file.
 *****************************************************************************/
void untar(const char * filename)
{
	printf("[extract `%s' ", filename);
	int fd = open(filename, O_RDWR);
	assert(fd != -1);

	char buf[SECTOR_SIZE * 16];
	int chunk = sizeof(buf);

	while (1) {
		read(fd, buf, SECTOR_SIZE);
		if (buf[0] == 0)
			break;

		struct posix_tar_header * phdr = (struct posix_tar_header *)buf;

		/* calculate the file size */
		char * p = phdr->size;
		int f_len = 0;
		while (*p)
			f_len = (f_len * 8) + (*p++ - '0'); /* octal */

		int bytes_left = f_len;
		int fdout = open(phdr->name, O_CREAT | O_RDWR);
		if (fdout == -1) {
			printf("    failed to extract file: %s\n", phdr->name);
			printf(" aborted]");
			return;
		}
		printf("    %s (%d bytes) ", phdr->name, f_len);
		while (bytes_left) {
			int iobytes = min(chunk, bytes_left);
			read(fd, buf,
			     ((iobytes - 1) / SECTOR_SIZE + 1) * SECTOR_SIZE);
			write(fdout, buf, iobytes);
			bytes_left -= iobytes;
		}
		close(fdout);
	}

	close(fd);

	printf(" done]\n");
}



/*****************************************************************************
 *                                Init
 *****************************************************************************/
/**
 * The hen.
 * 
 *****************************************************************************/
void Init()
{
	int fd_stdin  = open("/dev_tty0", O_RDWR);
	assert(fd_stdin  == 0);
	int fd_stdout = open("/dev_tty0", O_RDWR);
	assert(fd_stdout == 1);

	
	untar("/cmd.tar");
			

	char * tty_list[] = {"/dev_tty0"};

	int i;
	for (i = 0; i < sizeof(tty_list) / sizeof(tty_list[0]); i++) {
		int pid = fork();
		if (pid != 0) { /* parent process */
		}
		else {	/* child process */
			close(fd_stdin);
			close(fd_stdout);
			
			shabby_shell(tty_list[i]);
			assert(0);
		}
	}

	while (1) {
		int s;
		int child = wait(&s);
		printf("child (%d) exited with status: %d.\n", child, s);
	}

	assert(0);
}


/*======================================================================*
                               TestA
 *======================================================================*/
void TestA()
{
	//int fp=0;
        //fp = open("Test",O_CREAT | O_RDWR);
	//if(fp==-1)
 	//printf("TestA");
	//close(fp);
	//fp = open("Test",O_RDWR);
	//write(fp,"ceshi",5);
	//read(fp,arr,5);
	//close(fp);
	while(1)
	{		
	//char arr[5]="";
	
	//printf("TESTA");
	//printl("%s",arr);
		
	}
}

/*======================================================================*
                               TestB
 *======================================================================*/
void TestB()
{
	for(;;);
}

/*======================================================================*
                               TestB
 *======================================================================*/
void TestC()
{
	for(;;);
}

/*****************************************************************************
 *                                panic
 *****************************************************************************/
PUBLIC void panic(const char *fmt, ...)
{
	int i;
	char buf[256];

	/* 4 is the size of fmt in the stack */
	va_list arg = (va_list)((char*)&fmt + 4);

	i = vsprintf(buf, fmt, arg);

	printl("%c !!panic!! %s", MAG_CH_PANIC, buf);

	/* should never arrive here */
	__asm__ __volatile__("ud2");
}

/*****************************************************************************
 *                                xiaohong_shell
 *****************************************************************************/
/**
 * A very very powerful shell.
 * 
 * @param tty_name  TTY file name.
 *****************************************************************************/
void shabby_shell(const char * tty_name)
{

	
	int fd_stdin  = open(tty_name, O_RDWR);
	assert(fd_stdin  == 0);
	int fd_stdout = open(tty_name, O_RDWR);
	assert(fd_stdout == 1);

	char rdbuf[128]; 
	char cmd[128];
	char arg1[128];
        char arg2[128]; 
	char buf[1024]; 
	int j = 0; 

	//colorful();
	//clear();
	//animation();
	//welcome();
	printf("press any key to start:\n");
	int r = read(0, rdbuf, 70);

	int fq = open("Usr1", O_CREAT | O_RDWR);
	close(fq);
        initFS(); 

	while (1) {

		clearArr(rdbuf, 128);
        	clearArr(cmd, 128);
        	clearArr(arg1, 128);
        	clearArr(arg2, 128);
        	clearArr(buf, 1024);
		
		printf("%s $ ", location);

		int r = read(0, rdbuf, 70);
		rdbuf[r] = 0;
	

		int argc = 0;
		char * argv[PROC_ORIGIN_STACK];
		char * p = rdbuf;
		char * s;
		int word = 0;
		char ch;
		
		do {
			ch = *p;
			if (*p != ' ' && *p != 0 && !word) {
				s = p;
				word = 1;
			}
			if ((*p == ' ' || *p == 0) && word) {
				word = 0;
				argv[argc++] = s;
				*p = 0;
			}
			p++;
		} while(ch);
		argv[argc] = 0;

		int fd = open(argv[0], O_RDWR);
		if (fd == -1) {
		  //	printf("fd == -1\n");
			if (rdbuf[0]) {
				int i = 0, j = 0;
				/* get command */
				while (rdbuf[i] != ' ' && rdbuf[i] != 0)
				{
					cmd[i] = rdbuf[i];
					i++;
				}
				i++;
				/* get arg1 */
				while(rdbuf[i] != ' ' && rdbuf[i] != 0)
        			{
            				arg1[j] = rdbuf[i];
            				i++;
            				j++;
        			}
        			i++;
        			j = 0;
				/* get arg2 */
       				while(rdbuf[i] != ' ' && rdbuf[i] != 0)
        			{
            				arg2[j] = rdbuf[i];
            				i++;
            				j++;
        			}
				//	printf("%s\n",cmd);
				//	printf("%s\n",arg1);
				//	printf("%s\n",arg2);
				/* cmd */
				 if(strcmp(cmd, "what") == 0)
				{      
					printf("what function success\n");
				}
				if(strcmp(cmd, "login")==0)
				{
				login(arg1,arg2);
				}
				if(strcmp(cmd,"createFile")==0)
				  {
				     createFile(arg1);
				  }
				if(strcmp(cmd,"testArr")==0)
				  {
				   
				    printf("location:      %s\n",location);
				    printf("fileNames:\n");
			        
				    for(i=0;i<20;i++)
				      printf("%s | ",fileNames[i]);
				    printf("FAT:\n");

				    for(i=0;i<5;i++)
				      {
					for(j=0;j<5;j++)
					  printf("%d",FAT[i][j]);
					printf("\n");
				      }
				    printf("current file nums %d\n",fileCount);
				    
				  }

			}
		}
		else {
			close(fd);
			int pid = fork();
			if (pid != 0) { /* parent */
				int s;
				wait(&s);
			}
			else {	/* child */
				execv(argv[0], argv);
			}
		}
	}

	close(1);
	close(0);
}

/* Tools */

	/* Init Arr */
void clearArr(char *arr, int length)
{
    int i;
    for (i = 0; i < length; i++)
        arr[i] = 0;
}


void login(char * userName, char * passWord)
{
	printf("login");
	int i=0;
	for( i=0;i<2;i++)
	   { 
		if(strcmp(userName,users[i])==0 && strcmp(passWord,passwords[i])==0)
			{
                           printf("welcome!    %s",userName);
			  // printf("%d",strlen(location));
			   break; 
                           }
            }	
}

/* Init FS */
void initFS()
{
  int fp=-1;
 
  /* init users */
  
  fp=open("Usr1", O_RDWR);
  assert(fp!=-1);
  // write(fp,"sjw",3);
  printf("%d",fp);
  close(fp);
  
  clearArr(fileNames[1],strlen(fileNames[1]));
  strcpy(fileNames[1],"Usr1");
}


int getPos()
{
  int i=1;
  for(;i<fileCount;i++)
    {
      if(strcmp(fileNames[i],curFile)==0)
	return i;
    }
  printf("getPos\n");
  return 0;
}

void createFile(char * fileName)
{
  int fp=-1;
  int curPos=0;                               //current file's position in FAT

  curPos=getPos();
  if(curPos==0)
    printf("curPos=0\n");
  else
    {
      printf("curPos:  %d\n",curPos);
      int i=1;
      while(FAT[curPos][i]!=0  && i<5){
  	i++;
      };
      
      if(i==5)
  	printf("%d file has been full",curPos);
      else
  	{
	  fp=open(fileName,O_CREAT);
	  assert(fp!=-1);
	  printf("create %s fp:%d \n",fileName,fp);
	  close(fp);
	  fileCount++;
	  strcpy(fileNames[fileCount],fileName);       //  add File in Filenames
  	  FAT[curPos][i]=fileCount;
  	}
    }


}














PUBLIC int TESTA(char * topic)
{
	MESSAGE msg;
	reset_msg(&msg);
	msg.type = XIA;
	strcpy(msg.content, topic);
	printf("%s\n", msg.content);
	send_recv(BOTH, TASK_SYS, &msg);
	return msg.RETVAL;
}

void pwd(){}

