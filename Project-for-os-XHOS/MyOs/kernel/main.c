
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
#include "console.h"
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
char filepath[128] = "";
char users[2][128] = {"empty", "empty"};
char passwords[10][128];
char files[20][128];
char userfiles[20][128];
int filequeue[50];
int filecount = 0;
int usercount = 0;
int leiflag = 0;

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

	//int fq = open("User", O_CREAT | O_RDWR);
	//close(fq);
	//initFs();

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
			printf("fd == -1\n");
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
				printf("%s",cmd);
				/* cmd */
				 if(strcmp(cmd, "what") == 0)
				{
					printf("what function success\n");
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













/* Init FS */
void initFs()
{
	int fd = -1, n = 0, i = 0, count = 0, k = 0;
	char bufr[1024] = "";
	char bufp[1024] = "";
	char buff[1024] = "";

	for (i = 0; i < 500; i++)
		filequeue[i] = 1;

	fd = open("myUsers", O_CREAT | O_RDWR);
	close(fd);
	fd = open("myUsersPassword", O_CREAT | O_RDWR);
	close(fd);
	fd = open("fileLogs", O_CREAT | O_RDWR);
	close(fd);
	fd = open("user1", O_CREAT | O_RDWR);
	close(fd);
	fd = open("user2", O_CREAT | O_RDWR);
	close(fd);
	/* init users */
	fd = open("myUsers", O_RDWR);
        printf("befor read");
	n = read(fd, bufr, 1024);
	printf("%s",bufr);
	printf("read success");
	bufr[strlen(bufr)] = '\0';
	for (i = 0; i < strlen(bufr); i++)
	{
		if (bufr[i] != ' ')
		{
			users[count][k] = bufr[i];
			k++;
		}
		else
		{
			while (bufr[i] == ' ')
			{
				i++;
				if (bufr[i] == '\0')
				{
					users[count][k] = '\0';
					if (strcmp(users[count], "empty") != 0)
						usercount++;
					count++;
					break;
				}
			}
			if (bufr[i] == '\0')
			{
				break;
			}
			i--;
			users[count][k] = '\0';
			if (strcmp(users[count], "empty") != 0)
						usercount++;
			k = 0;
			count++;
		}
	}
	close(fd);
	count = 0;
	k = 0;
	
	/* init password */
	fd = open("myUsersPassword", O_RDWR);
	n = read(fd, bufp, 1024);
	for (i = 0; i < strlen(bufp); i++)
	{
		if (bufp[i] != ' ')
		{
			passwords[count][k] = bufp[i];
			k++;
		}
		else
		{
			while (bufp[i] == ' ')
			{
				i++;
				if (bufp[i] == '\0')
				{
					count++;
					break;
				}
			}
			if (bufp[i] == '\0')
				break;
			i--;
			passwords[count][k] = '\0';
			k = 0;
			count++;
		}
	}
	close(fd);
	count = 0;
	k = 0;

	/* init files */
	fd = open("fileLogs", O_RDWR);
	n = read(fd, buff, 1024);
	for (i = 0; i <= strlen(buff); i++)
	{
		if (buff[i] != ' ')
		{
			files[count][k] = buff[i];
			k++;
		}
		else
		{
			while (buff[i] == ' ')
			{
				i++;
				if (buff[i] == '\0')
				{
					break;
				}
			}
			if (buff[i] == '\0')
			{
				files[count][k] = '\0';
				count++;
				break;
			}
			i--;
			files[count][k] = '\0';
			k = 0;
			count++;
		}
	}
	close(fd);
	
	int empty = 0;
	for (i = 0; i < count; i++)
	{
		char flag[7];
		strcpy(flag, "empty");
		flag[5] = '0' + i;
		flag[6] = '\0';
		fd = open(files[i], O_CREAT | O_RDWR);
		close(fd);
	
		if (strcmp(files[i], flag) != 0)
			filequeue[i] = 0;
		else
			empty++;
	}
	filecount = count - empty;
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

