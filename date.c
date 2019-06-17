#include "types.h"
#include "user.h"
#include "date.h"

int main(int argc, char *argv[])
{
    struct rtcdate r;
    if (date(&r))
    {   /*第一个参数为标准错误*/
        printf(2, "date failed!!!\n");
        exit();
    }
    /*第一个参数为标准输出*/
    printf(1, "%d %d %d %d %d %d\n", r.year, r.month, r.day, r.hour, r.minute, r.second);
    exit();
}