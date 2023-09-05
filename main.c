#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h> // for bzero()
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include "sqlite3.h"

// #define DEV_RFID "/dev/ttySAC2"      //右上角第二排的串口
#define DEV_RFID "/dev/ttySAC2" // 右上角第一排的串口
#define FIFO "/tmp/myfifo"

bool first = true;

unsigned int readID();
int init_serial(int fd);
unsigned char CalBCCS(char *buf);
int PiccRequest(int fd);
unsigned int PiccAnticoll(int fd);

// 每当你使用SELECT得到N条记录时，就会自动调用N次以下函数
// 参数：
// arg: 用户自定义参数
// len: 列总数
// col_val: 每一列的值
// col_name: 每一列的名称（标题)
int showDB(void *arg, int len, char **col_val, char **col_name)
{
    if (arg != NULL)
    {
        (*(int *)arg)++; // n++;
        return 0;
    }

    printf("\n");
    /*for (int i = 0; i < len; i++)
    {
        printf("%s\t", col_name[0]);
    }*/

    printf("%s\t", col_name[0]);
    printf("\n==============");
    printf("==============\n");

    // 显示内容(一行一行输出)
    for (int i = 0; i < len; i++)
    {
        if (strcmp(col_name[i], "卡号") == 0)
        {
            unsigned int cardid = strtoul(col_val[i], NULL, 10);
            printf("0X%X\t", cardid);
        }
    }
    printf("\n");

    // 返回0: 继续针对下一条记录调用本函数
    // 返回非0: 停止调用本函数
    return 0;
}

int getTime(void *arg, int len, char **col_val, char **col_name)
{
    *(time_t *)arg = atol(col_val[0]);
    return 0;
}

#define ON 0
#define OFF 1

void beep(int n, float sec)
{
    static int fd;
    static int begin = true;
    if(begin)
    {
    	fd = open("/dev/beep", O_RDWR);
    	if(fd <= 0)
    	{
    	    perror("打开蜂鸣器失败");
    	    return;
    	}
    	else
        {
    	    begin = false;
        }
    }

    for(int i=0; i<n; i++)
    {
    	ioctl(fd, ON, 1);
    	usleep(sec*1000*1000);

    	ioctl(fd, OFF, 1);
    	usleep(sec*1000*1000);
    }
}

/*typedef struct Car
{
    char id[9];   // 卡号
    char str[30]; // 车牌
} car, pcar;*/

//bool Is_recv = false;

int main()
{
    // 判断管道文件是否存在
    if (access(FIFO, F_OK)) // 为真表示不存在
    {
        // 创建有名管道  管道文件不能放在共享文件夹中
        int ret = mkfifo(FIFO, 0777);
        if (ret < 0)
        {
            perror("mkfifo() failed\n");
            return -1;
        }
    }

    pid_t pid;
    if ((pid = fork()) == 0) // 子进程
    {
        // 打开管道文件
        int fd1 = open(FIFO, O_RDWR);
        if (fd1 < 0)
        {
            perror("open() failed\n");
            return -1;
        }

        int i = 0;
        char str[2][30]={0};
        char buf[30]={0};
        sprintf(str[0], "./alpr %d.jpg", 1);
        sprintf(str[1], "./alpr %d.jpg", 2);
        while (1)
        {
            bzero(buf,30);
            sleep(1);
            read(fd1,buf,30);
            if(strncmp(buf,"in",2)==0)
            {
                system(str[i]);
                i++;
            }
            else if(strncmp(buf,"out",3)==0)
            {
                i--;
            }
        }
    }
    else if (pid > 0) // 父进程
    {
        // 打开管道文件
        int fd2 = open(FIFO, O_RDWR);
        if (fd2 < 0)
        {
            perror("open() failed\n");
            return -1;
        }

        // 1，创建、打开一个数据库文件*.db
        sqlite3 *db = NULL;
        // int ret = sqlite3_open("parking.db", &db);
        int ret = sqlite3_open_v2("parking.db", &db,
                                  SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
        if (ret != SQLITE_OK)
        {
            printf("创建数据库文件失败:%s\n", sqlite3_errmsg(db));
            exit(0);
        }

        // 2，创建表Table
        char *err;
        sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS info (卡号 TEXT PRIMARY KEY, 时间 integer);",
                     NULL, NULL, &err);

        // 3，模拟车牌入库、出库
        char SQL[100];
        int cardid;
        int state = 0;

        unsigned int R_ID = 0;
        char ID[9] = {0};
        char buf[30]={0};

        while (1)
        {
            int n = 0;
            time_t t = 0;

            sleep(1);
            R_ID = readID();
            sprintf(ID, "%#X", R_ID);

            if (R_ID != 0)
            {
                // 判断卡号在数据库里面有没有
                bzero(SQL, 100);
                snprintf(SQL, 100, "SELECT * FROM info WHERE 卡号=%s;", ID);
                sqlite3_exec(db, SQL, showDB, &n /*用户自定义参数*/, &err);

                if (n == 0) // 如果没有，入卡号
                {
                    bzero(SQL, 100);
                    bzero(buf,30);
                    snprintf(SQL, 100, "INSERT INTO info VALUES(%s, '%lu');", ID, time(NULL));
                    sqlite3_exec(db, SQL, NULL, NULL, &err);
                    printf("入库成功\n");
                    strcpy(buf,"in");
                    write(fd2,buf,strlen(buf));//通过管道向图像识别算法发送信息
                    beep(1, 0.3);
                }
                else // 如果有，出卡号，并计算时间
                {
                    // B) 存在的情况下，计费并删除该记录
                    beep(1, 0.3);

                    bzero(SQL, 100);
                    bzero(buf,30);
                    strcpy(buf,"out");
                    write(fd2,buf,strlen(buf));//通过管道向图像识别算法发送信息

                    snprintf(SQL, 100, "SELECT * FROM info WHERE 卡号=%s;", ID);
                    sqlite3_exec(db, SQL, showDB, NULL, &err);

                    bzero(SQL, 100);
                    snprintf(SQL, 100, "SELECT 时间 FROM info WHERE 卡号=%s;", ID);
                    sqlite3_exec(db, SQL, getTime, &t, &err);

                    int seconds = time(NULL) - t;
                    // int fee = seconds < 30 ? 0 : (seconds / 30) * 2;

                    bzero(SQL, 100);
                    snprintf(SQL, 100, "DELETE FROM info WHERE 卡号=%s;", ID);
                    sqlite3_exec(db, SQL, NULL, NULL, &err);

                    printf("停车时长 %d 分钟,收费 %d 元\n", seconds, seconds);

                    printf("出库成功\n");
                }

                // 显示当前数据库内的内容
                bzero(SQL, 100);
                snprintf(SQL, 100, "SELECT * FROM info;");
                first = true;
                sqlite3_exec(db, SQL, showDB, NULL /*用户自定义参数*/, &err);
            }
        }
    }
    else if (pid < 0)
    {
        perror("fork() failed\n");
        return -1;
    }
}

unsigned int readID()
{
    // 打开连接RFID模块的串口
    int fd = open(DEV_RFID, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
    {
        perror("open serial:");
        return 0;
    }
    // 1、串口初始化
    init_serial(fd);

    // 2、请求   天线范围的卡
    if (PiccRequest(fd) != 0)
    {
        return 0;
    }

    // 3、进行  防碰撞处理  ，获取卡号
    unsigned int ID;
    if ((ID = PiccAnticoll(fd)) == 0)
    {
        // 关闭文件
        close(fd);
        return 0;
    }
    else
    {
        // 关闭文件
        close(fd);
        return ID;
    }
}

int init_serial(int fd)
{
    // 1.定义一个存放串口配置的结构体变量
    struct termios old_cfg, new_cfg;
    bzero(&old_cfg, sizeof(struct termios));
    bzero(&new_cfg, sizeof(struct termios));

    // 2.获取原有配置
    tcgetattr(fd, &old_cfg);

    // 3.激活选项
    new_cfg.c_cflag |= CLOCAL | CREAD;
    cfmakeraw(&new_cfg);

    // 4.设置波特率
    cfsetispeed(&new_cfg, B9600);
    cfsetospeed(&new_cfg, B9600);

    // 5.设置字符大小
    new_cfg.c_cflag &= ~CSIZE;
    new_cfg.c_cflag |= CS8;

    // 6.设置奇偶校验
    new_cfg.c_cflag &= ~PARENB;

    // 7.设置停止位
    new_cfg.c_cflag &= ~CSTOPB;

    // 8.设置最小字符和等待时间
    new_cfg.c_cc[VTIME] = 0;
    new_cfg.c_cc[VMIN] = 1;

    // 9.清除串口缓冲
    tcflush(fd, TCIFLUSH);

    // 10.激活配置
    if ((tcsetattr(fd, TCSANOW, &new_cfg)) != 0)
    {
        perror("tcsetattr");
        return -1;
    }

    return 0;
}

unsigned char CalBCCS(char *buf)
{
    int i;
    unsigned char BCC = 0;
    for (i = 0; i < buf[0] - 2; ++i)
    {
        BCC ^= buf[i];
    }
    return ~BCC;
}

int PiccRequest(int fd)
{
    // 定义一个请求数据帧
    unsigned char Wbuf[128], Rbuf[128];
    bzero(Wbuf, 128);
    bzero(Rbuf, 128);

    Wbuf[0] = 0x07;
    Wbuf[1] = 0x02;
    Wbuf[2] = 'A'; // 0x41
    Wbuf[3] = 0x01;
    Wbuf[4] = 0x52;
    Wbuf[5] = CalBCCS(Wbuf);
    Wbuf[6] = 0x03;

    // IO复用
    fd_set rdfd; // 定义一个IO描述符集合
    FD_ZERO(&rdfd);
    FD_SET(fd, &rdfd);

    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    // 发送请求数据帧
    write(fd, Wbuf, 7);

    int ret = select(fd + 1, &rdfd, NULL, NULL, &timeout);

    switch (ret)
    {
    case -1:
        printf("select 出错\n");
        break;
    case 0:
        printf("timeout 请求超时\n");
        break;
    default:
        ret = read(fd, Rbuf, 128);
        if (ret < 0)
        {
            printf("请求应答，读取失败\n");
            return -1;
        }
        else if (Rbuf[2] == 0x00) // 请求成功
        {
            // printf("请求成功\n");
            return 0;
        }
    }
    return -1;
}

unsigned int PiccAnticoll(int fd)
{
    // 定义一个防碰撞数据帧
    char Wbuf[128], Rbuf[128];
    bzero(Wbuf, 128);
    bzero(Rbuf, 128);

    Wbuf[0] = 0x08;
    Wbuf[1] = 0x02;
    Wbuf[2] = 'B'; // 0x42
    Wbuf[3] = 0x02;
    Wbuf[4] = 0x93;
    Wbuf[5] = 0x00;
    Wbuf[6] = CalBCCS(Wbuf);
    Wbuf[7] = 0x03;

    // IO复用
    fd_set rdfd; // 定义一个IO描述符集合
    FD_ZERO(&rdfd);
    FD_SET(fd, &rdfd);

    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    // 发送防碰撞数据帧
    write(fd, Wbuf, 8);

    int ret = select(fd + 1, &rdfd, NULL, NULL, &timeout);

    switch (ret)
    {
    case -1:
        printf("select 出错\n");
        break;
    case 0:
        printf("timeout 请求超时\n");
        break;
    default:
        ret = read(fd, Rbuf, 128);
        if (ret < 0)
        {
            printf("请求应答，读取失败\n");
            return 0;
        }
        else if (Rbuf[2] == 0x00) // 请求成功
        {
            unsigned int ID = Rbuf[7] << 24 | Rbuf[6] << 16 | Rbuf[5] << 8 | Rbuf[4] << 0;
            return ID;
        }
    }
    return 0;
}