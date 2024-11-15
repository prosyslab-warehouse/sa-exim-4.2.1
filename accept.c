/*************************************************
*     Exim - an Internet mail transport agent    *
*************************************************/

/* Copyright (c) University of Cambridge 1995 - 2002 */
/* See the file NOTICE for conditions of use and distribution. */


#include "local_scan.h"

/*
 * This is a basic version of local_scan that always accepts the messge.
 * It is like the template provided by Philip Hazel, except it is
 * intended to be compiled as a .so and loaded dynamically by the "real"
 * local_scan.
 */

int local_scan_version_major(void)
{
    return LOCAL_SCAN_ABI_VERSION_MAJOR;
}

int local_scan_version_minor(void)
{
    return LOCAL_SCAN_ABI_VERSION_MINOR;
}

int local_scan_version( void )
{
    return 1;
}
 

int local_scan(int fd, uschar **return_text)
{
    /* Keep pedantic compilers happy */
    fd = fd;
    return_text = return_text;

    log_write(0, LOG_MAIN, "Message accepted by dynamically loaded dummy local_scan");
    return LOCAL_SCAN_ACCEPT;
}


/* End of local_scan.c */
