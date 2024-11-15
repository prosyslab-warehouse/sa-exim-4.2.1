/* Spamassassin in local_scan by Marc MERLIN <marc_soft@merlins.org> */
/* $Id: sa-exim.c,v 1.71 2005/03/08 20:39:51 marcmerlin Exp $ */
/*

The inline comments and minidocs were moved to the distribution tarball

You can get the up to date version of this file and full tarball here:
http://sa-exim.sourceforge.net/
http://marc.merlins.org/linux/exim/sa.html
The discussion list is here:
http://lists.merlins.org/lists/listinfo/sa-exim
*/



#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "sa-exim.h"

/* Exim includes */
#include <local_scan.h>

#ifdef DLOPEN_LOCAL_SCAN

/* Karsten Engelke <me@kaeng.org> says this is missing on openbsd */
#ifndef RTLD_NOW
#define RTLD_NOW 0x002
#endif    

/* Return the verion of the local_scan ABI, if being compiled as a .so */
int local_scan_version_major(void)
{
    return LOCAL_SCAN_ABI_VERSION_MAJOR;
}

int local_scan_version_minor(void)
{
    return LOCAL_SCAN_ABI_VERSION_MINOR;
}

/* Left over for compatilibility with old patched exims that didn't have
   a version number with minor an major. Keep in mind that it will not work
   with older exim4s (I think 4.11 is required) */
#ifdef DLOPEN_LOCAL_SCAN_OLD_API
int local_scan_version(void)
{
    return 1;
}
#endif
#endif

#ifndef SAFEMESGIDCHARS
#define SAFEMESGIDCHARS "!#%( )*+,-.0123456789:<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]^_abcdefghijklmnopqrstuvwxyz{|}~";
#endif


/******************************/
/* Compile time config values */
/******************************/
#ifndef SPAMC_LOCATION
#define SPAMC_LOCATION	    "/usr/bin/spamc"
#endif

#ifndef SPAMASSASSIN_CONF
#define SPAMASSASSIN_CONF   "/etc/exim4/sa-exim.conf"
#endif
static const char conffile[]=SPAMASSASSIN_CONF;


/********************/
/* Code starts here */
/********************/
static const char nospamstatus[]="<error finding status>";

static char *buffera[4096];
static char *buffer=(char *)buffera;
static int SAEximDebug=0;
static int SAPrependArchiveWithFrom=1;
static jmp_buf jmp_env;

static char *where="Error handler called without error string";
static int line=-1;
static char *panicerror;

#define MIN(a,b) (a<b?a:b)

#define CHECKERR(mret, mwhere, mline) \
    if (mret < 0) \
    { \
        where=mwhere; \
        line=mline; \
        goto errexit; \
    }

#define PANIC(merror) \
    panicerror=merror; \
    goto panicexit;


static void alarm_handler(int sig)
{
    sig = sig;    /* Keep picky compilers happy */
    longjmp(jmp_env, 1);
}


/* Comparing header lines isn't fun, especially since the comparison has to
   be caseless, so we offload this to this function
   You can scan on partial headers, just give the root to scan for
   Return 1 if the header was found, 0 otherwise */
static int compare_header(char *buffertocompare, char *referenceheader)
{
    int idx;
    int same=1;

    for (idx=0; idx<strlen(referenceheader); idx++)
    {
	if ( tolower(referenceheader[idx]) != tolower(buffertocompare[idx]) )
	{
	    same=0;
	    break;
	}
    }

    if (SAEximDebug > 7)
    {
	if (same)
	{
	    log_write(0, LOG_MAIN, "SA: Debug8: Found %s in %s", referenceheader, buffertocompare);
	}
	else if (SAEximDebug > 8)
	{
	    log_write(0, LOG_MAIN, "SA: Debug9: Did not find %s in %s", referenceheader, buffertocompare);
	}
    }

    return same;
}


/* returns a header from a buffer line */
static char *get_header(char *buffer)
{
    char *start;
    char *end;
    char *header;

    start=buffer;
    end=strstr(buffer, ":");

    header=string_copyn(start, end-start);

    if (SAEximDebug>5)
    {
	log_write(0, LOG_MAIN, "SA: Debug6: Extracted header %s in buffer %s", header, buffer);
    }

    return header;
}


/* Rejected mails can be archived in a spool directory */
/* filename will contain a double / before the filename, I prefer two to none */
static int savemail(int readfd, off_t fdstart, char *dir, char *dirvarname, 
			char *filename, int SAmaxarchivebody, char *condition)
{
    header_line *hl;
    int writefd=0;
    int ret;
    ssize_t stret;
    off_t otret;
    char *expand;
    char *fake_env_from;
    int towrite;
    int chunk;
    struct stat bufst;

    if (dir == NULL)
    {
	if (SAEximDebug>4)
	{
	    log_write(0, LOG_MAIN, "SA: Debug5: Not saving message because %s in undefined", dirvarname);
	}
	return 0;
    }
    
    if (condition[0] != '1' || condition[1] != 0)
    {
	expand=expand_string(condition);
	if (expand == NULL)
	{
	    /* Can't use PANIC within this function :( */
	    CHECKERR(-1, string_sprintf("savemail condition expansion failure on %s", condition), __LINE__ - 1);
	}

	if (SAEximDebug > 2)
	{
	    log_write(0, LOG_MAIN, "SA: Debug3: savemail condition expand returned: '%s'", expand);
	}

	if (expand[0] == 0 || (expand[0] == '0' && expand[1] == 0))
	{
	    if (SAEximDebug > 1)
	    {
		log_write(0, LOG_MAIN, "SA: Debug2: savemail condition expanded to false, not saving message to disk");
	    }
	    return 0;
	}
    }

    if (SAEximDebug)
    {
	log_write(0, LOG_MAIN, "SA: Debug: Writing message to %s/new/%s", dir, filename);

    }

    if (stat(string_sprintf("%s/new/", dir), &bufst) == -1)
    {
	log_write(0, LOG_MAIN, "SA: Notice: creating maildir tree in  %s", dir);
	if (stat(dir, &bufst) == -1)
	{
	    ret=mkdir (dir, 0770);
	    CHECKERR(ret,string_sprintf("mkdir %s", dir),__LINE__);
	}
	ret=mkdir (string_sprintf("%s/new", dir), 0770);
	CHECKERR(ret,string_sprintf("mkdir %s/new/", dir),__LINE__);
	ret=mkdir (string_sprintf("%s/cur", dir), 0770);
	CHECKERR(ret,string_sprintf("mkdir %s/cur/", dir),__LINE__);
	ret=mkdir (string_sprintf("%s/tmp", dir), 0770);
	CHECKERR(ret,string_sprintf("mkdir %s/tmp/", dir),__LINE__);
    }
    
    /* Let's not worry about you receiving two spams at the same second
     * with the same message ID. If you do, the second one will overwrite
     * the first one */
    writefd=creat(string_sprintf("%s/new/%s", dir, filename), S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
    CHECKERR(writefd, string_sprintf("creat %s/new/%s", dir, filename),__LINE__);

    /* make the file look like a valid mbox -- idea from dman */
    /* Although now that we use maildir format, this isn't really necessary */
    /* Richard Lithvall made this an option */
    if(SAPrependArchiveWithFrom == 1)
    {
	fake_env_from=string_sprintf("From %s Thu Jan  1 00:00:01 1970\n",sender_address);
	stret=write(writefd, fake_env_from, strlen(fake_env_from));
	CHECKERR(stret,string_sprintf("'From ' line write in %s", filename),__LINE__);
    }

    /* First we need to get the header lines from exim, and then we can read
       the body from writefd */
    hl=header_list;
    while (hl != NULL)
    {
	/* type '*' means the header is internal, don't print it */
	if (hl->type == '*')
	{
	    hl=hl->next;
	    continue;
	}
	stret=write(writefd,hl->text,strlen(hl->text));
	CHECKERR(stret,string_sprintf("header line write in %s", filename),__LINE__);
	hl=hl->next;
    }
    stret=write(writefd,"\n",1);
    CHECKERR(stret,string_sprintf("header separation write in %s", filename),__LINE__);
    
    /* Now copy the body to the save file */
    /* we already read from readfd, so we need to reset it */
    otret=lseek(readfd, fdstart, SEEK_SET);
    CHECKERR(otret, "lseek reset on spooled message", __LINE__);

    if (SAEximDebug > 8)
    { 
	log_write(0, LOG_MAIN, "SA: Debug9: Archive body write starts: writing up to %d bytes in %d byte blocks", SAmaxarchivebody, sizeof(buffera));
    }

    towrite=SAmaxarchivebody;
    chunk=0;
    while (towrite>0 && (stret=read(readfd, buffer, MIN(sizeof(buffera), towrite))) > 0)
    {
	chunk++;
	if (SAEximDebug > 8)
	{ 
	    log_write(0, LOG_MAIN, "SA: Debug9: Processing archive body chunk %d (read %.0f, and %.0f can still be written)", chunk, (double)stret, (double)towrite);
	}
	towrite-=stret;
	stret=write(writefd, buffer, stret);
	CHECKERR(stret,string_sprintf("body write in %s", filename),__LINE__);
    }
    CHECKERR(stret, "read body for archival", __LINE__ - 8);
    ret=close(writefd);
    CHECKERR(ret, "Closing spooled message",__LINE__);
    return 0;

    /* catch the global errexit, clean up, and return the error up */
    errexit:
    close(writefd);
    return -1;
}

/*
 * let's add the X-SA-Exim-Connect-IP, X-SA-Exim-Rcpt-To, and 
 * X-SA-Exim-Mail-From headers.
 * Those are all required by the greylisting with SA implementation
 * And From/Rcpt-To can also be used for personalized SA rules
 */
void AddSAEheaders(char *rcptlist, int SAmaxrcptlistlength)
{
    if (sender_host_address)
    {
	header_add(' ', "X-SA-Exim-Connect-IP: %s\n", sender_host_address);
    }
    else
    {
	header_add(' ', "X-SA-Exim-Connect-IP: <locally generated>\n");
    }

    /* Create a mega envelope-to header with all the recipients */
    /* Note, if you consider this a privacy violation, you can remove the header
     * in exim's system filter.
     * This is very useful to see who a message was really sent to, and can
     * be used by Spamassassin to do additional scoring */
    if (strlen(rcptlist) <= SAmaxrcptlistlength)
    {
	header_add(' ', "X-SA-Exim-Rcpt-To: %s\n", rcptlist);
    }
    /* Therefore SAmaxrcptlistlength set to 0 disables the header completely */
    else if (SAmaxrcptlistlength)
    {
	header_add(' ', "X-SA-Exim-Rcpt-To: too long (recipient list exceeded maximum allowed size of %d bytes)\n", SAmaxrcptlistlength);
    }

    header_add(' ', "X-SA-Exim-Mail-From: %s\n", sender_address);
}

void RemoveHeaders(char *headername)
{
    header_line *hl;

    /* Remove headers that SA can set */
    hl=header_list;
    while (hl != NULL)
    {

	/* type '*' means the header is internal or deleted */
	if (hl->type == '*')
	{
	    hl=hl->next;
	    continue;
	}

	/* Strip all SA and SA-Exim headers on incoming mail */
	if ( compare_header((char *)hl->text, headername) )
	{
	    if (SAEximDebug > 2)
	    {
		log_write(0, LOG_MAIN, "SA: Debug3: removing header %s on incoming mail '%s'", headername, (char *)hl->text);
	    }
	    hl->type = '*';
	}
	hl=hl->next;
    }
}


/* 
 * Headers can be multi-line (in theory all of them can I think). Parsing them
 * is a little more work than a simple line scan, so we're off-loading this to
 * a function
 */
int parsemlheader(char *buffer, FILE *readfh, char *headername, char **header)
{
    header_line *hl;
    char *dummy;
    char *foundheadername;

    if (SAEximDebug > 4)
    {
	log_write(0, LOG_MAIN, "SA: Debug5: looking for header %s", headername);
    }

    if (header == NULL)
    {
	header=&dummy;
    }

    if (compare_header(buffer, string_sprintf("%s", headername)))
    {
	*header=string_copy(buffer);

	/* Read the next line(s) in case this is a multi-line header */
	while ((fgets((char *)buffer,sizeof(buffera),readfh)) != NULL)
	{
	    /* Remove trailing newline */
	    if (buffer[strlen(buffer)-1] == '\n')
	    {
		buffer[strlen(buffer)-1]=0;
		/* and any carriage return */
		if (buffer[strlen(buffer)-1] == '\r')
		{
		    buffer[strlen(buffer)-1]=0;
		}
	    }
	    if (SAEximDebug > 5)
	    {
		log_write(0, LOG_MAIN, "SA: Debug6: while parsing header %s, read %s", headername, buffer);
	    }
	    /* concatenated lines only start with space or tab. right? */
	    if (buffer[0] != ' ' && buffer[0] != '\t')
	    {
		break;
	    }

	    /* Guard against humongous header lines */
	    if (strlen(*header) < 8000)
	    {
		/* Slight waste of memory here, oh well... */
		*header=string_sprintf("%s\n%s", *header, buffer);
	    }
	    else
	    {
		log_write(0, LOG_MAIN, "SA: Warning: while parsing header %s, ignoring the following trailing line due to header size overflow: %s", headername, buffer);

	    }
	}
	if (SAEximDebug > 5)
	{
	    log_write(0, LOG_MAIN, "SA: Debug6: header pieced up %s as: '%s'", headername, *header);
	}

	/* Headers need a newline at the end before being handed out to exim */
	/* Slight waste of memory here, oh well... */
	*header=string_sprintf("%s\n", *header);

	foundheadername=get_header(*header);

	/* Mark the former header as deleted if it's already present */
	/* Note that for X-Spam, it won't since we already deleted it earlier */
	hl=header_list;
	while (hl != NULL)
	{
	    /* type '*' means the header is internal or deleted */
	    if (hl->type == '*')
	    {
		hl=hl->next;
		continue;
	    }

	    if ( compare_header((char *)hl->text, foundheadername) )
	    {
		if (SAEximDebug > 5)
		{
		    log_write(0, LOG_MAIN, "SA: Debug6: removing old copy of header '%s' and replacing with new one: '%s'", (char *)hl->text, *header);
		}
		hl->type = '*';
		break;
	    }   
	    hl=hl->next;
	}

	header_add(' ', "%s", *header);
	return 1;
    }
    return 0;
}


char *cleanmsgid(char *msgid, char *SAsafemesgidchars)
{
    char *safemesgid;
    char *ptr;

    /* In case the message-Id is too long, let's truncate it */
    safemesgid=string_copyn(msgid, 220);
    ptr=safemesgid;

    /* Clean Message-ID to make sure people can't write on our FS */
    while (*ptr)
    {
	/* This might be more aggressive than you want, but since you
	 * potentially have shell programs dealing with the resulting filenames
	 * let's make it a bit safer */
	if (strchr(SAsafemesgidchars, *ptr) == NULL)
	{
	    *ptr='_';
	}
	ptr++;
    }

    if (SAEximDebug > 1)
    {
	log_write(0, LOG_MAIN, "SA: Debug2: Message-Id taken from Exim and cleaned from: %s to: %s", msgid, safemesgid);
    }
    
    return safemesgid;
}


/* Exim calls us here, feeds us a fd on the message body, and expects a return
   message in *return_text */
int local_scan(volatile int fd, uschar **return_text)
{
#warning you should not worry about the "might be clobbered by longjmp", see source
    int ret;
    ssize_t stret;
    int pid;
    int writefd[2];
    int readfd[2];
    char *spamc_argv[10];
    int i;
    /* These are the only values that we want working after the longjmp 
     * The automatic ones can be clobbered, but we don't really care */
    volatile FILE *readfh;
    volatile char *mesgfn=NULL;
    volatile off_t fdsize;
    volatile off_t scansize;
    volatile off_t fdstart;
    volatile char *rcptlist;
    volatile void *old_sigchld;
    char *safemesgid=NULL;
    int isspam=0;
    int gotsa=0;
    int chunk;
    off_t towrite;
    char *mailinfo;
    float spamvalue=0.0;
    char *spamstatus=NULL;
    time_t beforescan;
    time_t afterscan;
    time_t afterwait;
    time_t scantime=0;
    time_t fulltime=0;
    struct stat stbuf;

    uschar *expand;
    header_line *hl;

    static int readconffile=0;
    static int wrotedebugenabled=0;

    /* Options we read from /etc/exim4/sa-exim.conf */
    static char *SAspamcpath=SPAMC_LOCATION;
    static char *SAsafemesgidchars=SAFEMESGIDCHARS
    static char *SAspamcSockPath=NULL;
    static char *SAspamcPort=NULL;
    static char *SAspamcHost=NULL;
    static char *SAspamcUser=NULL;
    static char *SAEximRunCond="0";
    static char *SAEximRejCond="1";
    static int SAmaxbody=250*1024;
    static char *SATruncBodyCond="0";
    static int SARewriteBody=0;
    static int SAmaxarchivebody=20*1048576;
    static int SAerrmaxarchivebody=1024*1048576;
    static int SAmaxrcptlistlength=0;
    static int SAaddSAEheaderBeforeSA=1;
    static int SAtimeout=240;
    static char *SAtimeoutsave=NULL;
    static char *SAtimeoutSavCond="1";
    static char *SAerrorsave=NULL;
    static char *SAerrorSavCond="1";
    static int SAtemprejectonerror=0;
    static char *SAteergrube="1048576";
    static float SAteergrubethreshold;
    /* This is obsolete, since SAteergrube (now a condition) can do the same */
    static char *SAteergrubecond="1";
    static int SAteergrubetime=900;
    static char *SAteergrubeSavCond="1";
    static char *SAteergrubesave=NULL;
    static int SAteergrubeoverwrite=1;
    static char *SAdevnull="1048576";
    static float SAdevnullthreshold;
    static char *SAdevnullSavCond="1";
    static char *SAdevnullsave=NULL;
    static char *SApermreject="1048576";
    static float SApermrejectthreshold;
    static char *SApermrejectSavCond="1";
    static char *SApermrejectsave=NULL;
    static char *SAtempreject="1048576";
    static float SAtemprejectthreshold;
    static char *SAtemprejectSavCond="1";
    static char *SAtemprejectsave=NULL;
    static int SAtemprejectoverwrite=1;
    static char *SAgreylistiswhitestr="GREYLIST_ISWHITE";
    static float SAgreylistraisetempreject=3.0;
    static char *SAspamacceptsave=NULL;
    static char *SAspamacceptSavCond="0";
    static char *SAnotspamsave=NULL;
    static char *SAnotspamSavCond="0";
    /* Those variables can take a %s to show the spam info */
    static char *SAmsgteergrubewait="wait for more output";
    static char *SAmsgteergruberej="Please try again later";
    static char *SAmsgpermrej="Rejected";
    static char *SAmsgtemprej="Please try again later";
    /* Do not put a %s in there, or you'll segfault */
    static char *SAmsgerror="Temporary local error while processing message, please contact postmaster";

    /* This needs to be retrieved through expand_string in order
       not to violate the API. */
    uschar *primary_hostname=expand_string("$primary_hostname");

    /* New values we read from spamassassin */
    char *xspamstatus=NULL;
    char *xspamflag=NULL;


    /* Any error can write the faulty message to mesgfn, so we need to
       give it a value right now. We'll set the real value later */
    /* message_id here comes from Exim, it's an internal disk Mesg-Id format
       which doesn't correlate to the actual message's Mesg-Id. We shouldn't
       need to clean it, and besides, SAsafemesgidchars hasn't been read from
       the config file yet, but eh, safety is always a good thing, right? */
    safemesgid=cleanmsgid(message_id, SAsafemesgidchars);
    mesgfn=string_sprintf("%d_%s", time(NULL), safemesgid);

    /* We won't scan local messages. I think exim bypasses local_scan for a
     * bounce generated after a locally submitted message, but better be safe */
    /* This is commented out now, because you can control it with SAEximRunCond
    if (!sender_host_address)
    {
	return LOCAL_SCAN_ACCEPT;
    } 
    */

    /* If you discard a mail with exim ACLs, we get 0 recipients, so let's just
     * accept the mail, which won't matter either way since it'll get dropped
     * (thanks to John Horne for reporting this corner case) */
    if (recipients_count == 0) 
    {
	return LOCAL_SCAN_ACCEPT;
    }

    /* 
     * We keep track of whether we've alrady read the config file, but since
     * exim spawns itself, it will get read by exim children even though you
     * didn't restart exim. That said, after you change the config file, you
     * should restart exim to make sure all the instances pick up the new 
     * config file
     */
    if (!readconffile)
    {
	ret=open(conffile, 0);
	CHECKERR(ret,string_sprintf("conf file open for %s", conffile),__LINE__);
	readfh=fdopen(ret, "r");
	CHECKERR(readfh,"fdopen",__LINE__);
	
	while ((fgets((char *)buffer, sizeof(buffera), (FILE *)readfh)) != NULL)
	{
	    if (*buffer == '#' || *buffer == '\n' )
	    {
		continue;
	    }
		
	    if (*buffer != 'S' || *(buffer+1) != 'A')
	    {
		log_write(0, LOG_MAIN, "SA: Warning: error while reading configuration file %s. Line does not begin with a SA directive: '%s', ignoring", conffile, buffer);
		continue;
	    }

#define     M_CHECKFORVAR(VAR, TYPE) \
	    if (strstr(buffer, #VAR ": ") == buffer) \
	    { \
		if (sscanf(buffer, #VAR ": " TYPE, &VAR)) \
		{ \
		    if (SAEximDebug > 3) \
		    { \
			if (SAEximDebug && ! wrotedebugenabled) \
			{ \
			    log_write(0, LOG_MAIN, "SA: Debug4: Debug enabled, reading config from file %s", conffile); \
			    wrotedebugenabled=1; \
			} \
			else \
			{ \
			    log_write(0, LOG_MAIN, "SA: Debug4: config read " #VAR " = " TYPE, VAR); \
			}\
		    }\
		} \
		else \
		{ \
		    log_write(0, LOG_MAIN, "SA: Warning: error while reading configuration file %s. Can't parse value in: '%s', ignoring", conffile, buffer); \
		} \
		continue; \
	    } 

#define	    M_CHECKFORSTR(VAR) \
	    if (strstr(buffer, #VAR  ": ") == buffer) \
	    { \
		VAR = strdup(buffer+strlen( #VAR )+2); \
		if (VAR == NULL) \
		{ \
		    log_write(0, LOG_MAIN, "SA: PANIC: malloc failed, quitting..."); \
		    exit(-1); \
		} \
		\
		if (VAR[strlen(VAR)-1] == '\n') \
		{ \
		    VAR[strlen(VAR)-1]=0; \
		} \
		if (SAEximDebug > 3) \
		{ \
		    log_write(0, LOG_MAIN, "SA: Debug4: config read " #VAR " = %s", VAR); \
		} \
		continue; \
	    } 

	    M_CHECKFORVAR(SAEximDebug, "%d");
	    M_CHECKFORSTR(SAspamcpath);
	    M_CHECKFORSTR(SAsafemesgidchars);
	    M_CHECKFORSTR(SAspamcSockPath);
	    M_CHECKFORSTR(SAspamcPort);
	    M_CHECKFORSTR(SAspamcHost);
	    M_CHECKFORSTR(SAspamcUser);
	    M_CHECKFORSTR(SAEximRunCond);
	    M_CHECKFORSTR(SAEximRejCond);
	    M_CHECKFORVAR(SAmaxbody, "%d");
	    M_CHECKFORSTR(SATruncBodyCond);
	    M_CHECKFORVAR(SARewriteBody, "%d");
	    M_CHECKFORVAR(SAPrependArchiveWithFrom, "%d");
	    M_CHECKFORVAR(SAmaxarchivebody, "%d");
	    M_CHECKFORVAR(SAerrmaxarchivebody, "%d");
	    M_CHECKFORVAR(SAmaxrcptlistlength, "%d");
	    M_CHECKFORVAR(SAaddSAEheaderBeforeSA, "%d");
	    M_CHECKFORVAR(SAtimeout, "%d");
	    M_CHECKFORSTR(SAtimeoutsave);
	    M_CHECKFORSTR(SAtimeoutSavCond);
	    M_CHECKFORSTR(SAerrorsave);
	    M_CHECKFORSTR(SAerrorSavCond);
	    M_CHECKFORVAR(SAtemprejectonerror, "%d");
	    M_CHECKFORSTR(SAteergrube);
	    M_CHECKFORSTR(SAteergrubecond);
	    M_CHECKFORVAR(SAteergrubetime, "%d");
	    M_CHECKFORSTR(SAteergrubeSavCond);
	    M_CHECKFORSTR(SAteergrubesave);
	    M_CHECKFORVAR(SAteergrubeoverwrite, "%d");
	    M_CHECKFORSTR(SAdevnull);
	    M_CHECKFORSTR(SAdevnullSavCond);
	    M_CHECKFORSTR(SAdevnullsave);
	    M_CHECKFORSTR(SApermreject);
	    M_CHECKFORSTR(SApermrejectsave);
	    M_CHECKFORSTR(SApermrejectSavCond);
	    M_CHECKFORSTR(SAtempreject);
	    M_CHECKFORSTR(SAtemprejectSavCond);
	    M_CHECKFORSTR(SAtemprejectsave);
	    M_CHECKFORVAR(SAtemprejectoverwrite, "%d");
	    M_CHECKFORSTR(SAgreylistiswhitestr);
	    M_CHECKFORVAR(SAgreylistraisetempreject, "%f");
	    M_CHECKFORSTR(SAspamacceptsave);
	    M_CHECKFORSTR(SAspamacceptSavCond);
	    M_CHECKFORSTR(SAnotspamsave);
	    M_CHECKFORSTR(SAnotspamSavCond);
	    M_CHECKFORSTR(SAmsgteergrubewait);
	    M_CHECKFORSTR(SAmsgteergruberej);
	    M_CHECKFORSTR(SAmsgpermrej);
	    M_CHECKFORSTR(SAmsgtemprej);
	    M_CHECKFORSTR(SAmsgerror);


	}
	
	readconffile=1;
    }

#define M_CONDTOFLOAT(VAR) \
    if ((expand=expand_string( VAR )) == NULL) \
    { \
	PANIC(string_sprintf(#VAR " config expansion failure on %s", #VAR ));\
    } \
    sscanf(expand, "%f", &VAR ## threshold); \
    if (SAEximDebug > 2) \
    { \
	log_write(0, LOG_MAIN, "SA: Debug3: expanded " #VAR " = %.2f", VAR ## threshold); \
    }\

    M_CONDTOFLOAT(SAteergrube);
    M_CONDTOFLOAT(SAdevnull);
    M_CONDTOFLOAT(SApermreject);
    M_CONDTOFLOAT(SAtempreject);

    /* Initialize the list of recipients here */
    rcptlist=string_copy(recipients_list[0].address);
    for (i=1; i < recipients_count && strlen((char *)rcptlist) < 7998 - strlen(recipients_list[i].address); i++)
    {
	rcptlist=string_sprintf("%s, %s", rcptlist, recipients_list[i].address);
    }

    if (sender_host_address != NULL)
    {
	mailinfo=string_sprintf("From <%s> (host=%s [%s]) for", 
		sender_address, sender_host_name, sender_host_address);
    }
    else
    {
	mailinfo=string_sprintf("From <%s> (local) for", sender_address);
    }
    mailinfo=string_sprintf("%s %s", mailinfo, rcptlist);


    /* Remove SA-Exim headers that could have been set before we add ours*/
    RemoveHeaders("X-SA-Exim-");

    if(SAaddSAEheaderBeforeSA)
    {
	AddSAEheaders((char *)rcptlist, SAmaxrcptlistlength);
    }

    /* This is used later if we need to rewind and save the body elsewhere */
    fdstart=lseek(fd, 0, SEEK_CUR);
    CHECKERR(fdstart,"lseek SEEK_CUR",__LINE__);

    ret=fstat(fd, &stbuf);
    CHECKERR(ret,"fstat fd",__LINE__);
    /* this is the body size plus a few bytes (exim msg ID) */
    /* it should be 18 bytes, but I'll assume it could be more or less */
    fdsize=stbuf.st_size;

    if (SAEximDebug > 3)
    {
	log_write(0, LOG_MAIN, "SA: Debug4: Message body is about %.0f bytes and the initial offset is %.0f", (double)(fdsize-18), (double)fdstart);
    }

    if (fdsize > SAmaxbody)
    {
	if (SATruncBodyCond[0] != '1' || SATruncBodyCond[1] != 0)
	{
	    expand=expand_string(SATruncBodyCond);
	    if (expand == NULL)
	    {
		PANIC(string_sprintf("SATruncBodyCond expansion failure on %s", SATruncBodyCond));
	    }

	    if (SAEximDebug)
	    {
		log_write(0, LOG_MAIN, "SA: Debug: SATruncBodyCond expand returned: '%s'", expand);
	    }

	    if (expand[0] == 0 || (expand[0] == '0' && expand[1] == 0))
	    {
		log_write(0, LOG_MAIN, "SA: Action: check skipped due to message size (%.0f bytes) and SATruncBodyCond expanded to false (Message-Id: %s). %s", (double)(fdsize-18), safemesgid, mailinfo);
		header_add(' ', "X-SA-Exim-Scanned: No (on %s); Message bigger than SAmaxbody (%d)\n", primary_hostname, SAmaxbody);
		return LOCAL_SCAN_ACCEPT;
	    }
	}

	if (SAEximDebug > 1)
	{
	    log_write(0, LOG_MAIN, "SA: Debug2: Message body is about %.0f bytes and SATruncBodyCond expanded to true, will feed a truncated body to SA", (double)(fdsize-18));
	}

	/* Let's feed exactly spamc will accept */
	scansize=SAmaxbody;
	header_add(' ', "X-SA-Exim-Scan-Truncated: Fed %.0f bytes of the body to SA instead of %.0f\n", (double)scansize, (double)fdsize);
    }
    else
    {
	scansize=fdsize;
    }

    expand=expand_string(SAEximRunCond);
    if (expand == NULL)
    {
	PANIC(string_sprintf("SAEximRunCond expansion failure on %s", SAEximRunCond));
    }

    if (SAEximDebug)
    {
	log_write(0, LOG_MAIN, "SA: Debug: SAEximRunCond expand returned: '%s'", expand);
    }


    /* Bail from SA if the expansion string says so */
    if (expand[0] == 0 || (expand[0] == '0' && expand[1] == 0))
    {
	log_write(0, LOG_MAIN, "SA: Action: Not running SA because SAEximRunCond expanded to false (Message-Id: %s). %s", safemesgid, mailinfo);
	header_add(' ', "X-SA-Exim-Scanned: No (on %s); SAEximRunCond expanded to false\n", primary_hostname);
	return LOCAL_SCAN_ACCEPT;
    }

    if (SAEximDebug)
    {
	log_write(0, LOG_MAIN, "SA: Debug: check succeeded, running spamc");
    }

    /* Ok, so now that we know we're running SA, we remove the X-Spam headers */
    /* that might have been there */
    RemoveHeaders("X-Spam-");

    
    beforescan=time(NULL);
    /* Fork off spamc, and get ready to talk to it */
    ret=pipe(writefd);
    CHECKERR(ret,"write pipe",__LINE__);
    ret=pipe(readfd);
    CHECKERR(ret,"read pipe",__LINE__);
    
    /* Ensure that SIGCHLD isn't being ignored. */
    old_sigchld = signal(SIGCHLD, SIG_DFL);

    if ((pid=fork()) < 0)
    {
	CHECKERR(pid, "fork", __LINE__ - 1);
    }	

    if (pid == 0)
    {
	close(readfd[0]);
	close(writefd[1]);

	ret=dup2(writefd[0],0);
	CHECKERR(ret,"dup2 stdin",__LINE__);
	ret=dup2(readfd[1],1);
	CHECKERR(ret,"dup2 stdout",__LINE__);
	ret=dup2(readfd[1],2);
	CHECKERR(ret,"dup2 stderr",__LINE__);

	i = 0;
	spamc_argv[i++] = "spamc";
	if (SAspamcUser && SAspamcUser[0])
	{
	    expand=expand_string(SAspamcUser);
	    if (expand == NULL)
	    {
		log_write(0, LOG_MAIN | LOG_PANIC, "SA: SAspamcUser expansion failure on %s, will run as Exim user instead.", SAspamcUser);
	    }
	    else if (expand[0] != '\0')
	    {
		spamc_argv[i++] = "-u";
		spamc_argv[i++] = expand;
	    }
	}

	/* 
         * I could implement the spamc protocol and talk to spamd directly
         * instead of forking spamc, but considering the overhead spent
         * in spamd, forking off spamc seemed acceptable rather than
         * re-implementing and tracking the spamc/spamd protocol or linking
	 * with a possibly changing library
	 */
	/* Ok, we cheat, spamc cares about how big the whole message is and
         * we only know about the body size, so I'll  give an extra 16K
         * to account for any headers that can accompany the message */

	spamc_argv[i++] = "-s";
	spamc_argv[i++] = string_sprintf("%d", SAmaxbody+16384);

	if(SAspamcSockPath)
	{
    	    spamc_argv[i++] = "-U";
	    spamc_argv[i++] = SAspamcSockPath;
	}
	else
	{
	    if (SAspamcHost) {
		spamc_argv[i++] = "-d";
		spamc_argv[i++] = SAspamcHost;
	    }
	    if (SAspamcPort) {
		spamc_argv[i++] = "-p";
		spamc_argv[i++] = SAspamcPort;
	    }
	}
	spamc_argv[i++] = NULL;

	ret=execv(SAspamcpath, spamc_argv);
	CHECKERR(ret,string_sprintf("exec %s", SAspamcpath),__LINE__);
    }

    if (SAEximDebug > 8)
    {
	log_write(0, LOG_MAIN, "SA: Debug9: forked spamc");
    }

    ret=close(readfd[1]);
    CHECKERR(ret,"close r",__LINE__);
    ret=close(writefd[0]);
    CHECKERR(ret,"close w",__LINE__);
    readfh=fdopen(readfd[0], "r");

    if (SAEximDebug > 8)
    {
	log_write(0, LOG_MAIN, "SA: Debug9: closed filehandles");
    }

    /* Ok, we're ready for spewing the mail at spamc */
    /* First we need to get the header lines from exim, and then we can read
       the body from fd */
    hl=header_list;
    while (hl != NULL)
    {
	/* type '*' means the header is internal, don't print it */
	if (hl->type == '*')
	{
	    hl=hl->next;
	    continue;
	}

	stret=write(writefd[1],hl->text,strlen(hl->text));
	CHECKERR(stret,"header line write",__LINE__);

	hl=hl->next;
    }
    stret=write(writefd[1],"\n",1);
    CHECKERR(stret,"header separation write",__LINE__);

    if (SAEximDebug > 6)
    {
	log_write(0, LOG_MAIN, "SA: Debug7: sent headers to spamc pipe. Sending body...");
    }

    towrite=scansize;
    chunk=0;
    while (towrite>0 && (stret=read(fd, buffer, MIN(sizeof(buffera), towrite))) > 0)
    {
	chunk++;
	if (SAEximDebug > 8)
	{ 
	    log_write(0, LOG_MAIN, "SA: Debug9: spamc body going to write chunk %d (read %.0f, %.0f left to write)", chunk, (double)stret, (double)towrite);
	}
	towrite-=stret;
	stret=write(writefd[1], buffer, stret);
	CHECKERR(stret,"body write in",__LINE__);
	if (SAEximDebug > 8)
	{ 
	    log_write(0, LOG_MAIN, "SA: Debug9: Spamc body wrote chunk %d (wrote %.0f, %.0f left to write)", chunk, (double)stret, (double)towrite);
	}
    }
    CHECKERR(stret, "read body", __LINE__ - 14);
    close(writefd[1]);

    if (SAEximDebug > 5)
    {
	log_write(0, LOG_MAIN, "SA: Debug6: fed spam to spamc, reading result");
    }
    
    if (SAtimeout)
    {
	if (SAEximDebug > 2)
	{
	    log_write(0, LOG_MAIN, "SA: Debug3: Setting timeout of %d secs before reading from spamc", SAtimeout);
	}
	/* SA can take very long to run for various reasons, let's not wait
	 * forever, that's just bad at SMTP time */
	if (setjmp(jmp_env) == 0)
	{
	    signal(SIGALRM, alarm_handler);
	    alarm (SAtimeout);
	}
	else
	{
	    /* Make sure that all your variables here are volatile or static */
	    signal(SIGCHLD, old_sigchld);
	    fclose((FILE *)readfh);

	    header_add(' ', "X-SA-Exim-Scanned: No (on %s); SA Timed out after %d secs\n", primary_hostname, SAtimeout);

	    /* We sent it to LOG_REJECT too so that we get a header dump */
	    log_write(0, LOG_MAIN | LOG_REJECT, "SA: Action: spamd took more than %d secs to run, accepting message (scanned in %d/%d secs | Message-Id: %s). %s", SAtimeout, scantime, fulltime, safemesgid, mailinfo);

	    ret=savemail(fd, fdstart, SAtimeoutsave, "SAtimeoutsave", (char *)mesgfn, SAerrmaxarchivebody, SAtimeoutSavCond);
	    CHECKERR(ret,where,line);

	    /* Make sure we kill spamc in case SIGPIPE from fclose didn't */
	    kill(pid, SIGTERM);
	    return LOCAL_SCAN_ACCEPT;

	}
    }

    /* Let's see what SA has to tell us about this mail and store the headers */
    while ((fgets((char *)buffer,sizeof(buffera),(FILE *) readfh)) != NULL)
    {
	/* Remove trailing newline */
	if (buffer[strlen(buffer)-1] == '\n')
	{
	    buffer[strlen(buffer)-1]=0;
	    /* and any carriage return */
	    if (buffer[strlen(buffer)-1] == '\r')
	    {
		buffer[strlen(buffer)-1]=0;
	    }
	}
restart:
	if (SAEximDebug > 5)
	{
	    log_write(0, LOG_MAIN, "SA: Debug6: spamc read: %s", buffer);
	}

	/* Let's handle special multi-line headers first, what a pain... */
	/* We feed the one line we read and the filehandle because we'll need
	   to check whether more lines need to be concatenated */
        /* This is ugly, there is an order dependency so we return to the
           beginning of the loop without reading a new line since we already
	   did that */
	if (parsemlheader(buffer, (FILE *)readfh, "Subject", NULL)) goto restart;
	if ((SARewriteBody == 1) && parsemlheader(buffer, (FILE *)readfh, "Content-Type", NULL)) goto restart;
	if ((SARewriteBody == 1) && parsemlheader(buffer, (FILE *)readfh, "Content-Transfer-Encoding", NULL)) goto restart;

	if (parsemlheader(buffer, (FILE *)readfh, "X-Spam-Flag", &xspamflag))
	{
	    if (xspamflag[13] == 'Y')
	    {
		isspam=1;
	    }
	    if (SAEximDebug > 2)
	    {
		log_write(0, LOG_MAIN, "SA: Debug3: isspam read from X-Spam-Flag: %d", isspam);
	    }
	    goto restart; 
	}

	if (parsemlheader(buffer, (FILE *)readfh, "X-Spam-Status", &xspamstatus))
	{
	    char *start;
	    char *end;

	    gotsa=1;

	    /* if we find the preconfigured greylist string (and it is defined
	     * in sa-exim.conf), we can raise the threshold for tempreject just
	     * for this mail, since it's been whitelisted */
	    if (SAgreylistiswhitestr && strstr(xspamstatus, SAgreylistiswhitestr))
	    {
	        SAtemprejectthreshold+=SAgreylistraisetempreject;
		if (SAEximDebug > 2)
		{
		    log_write(0, LOG_MAIN, "SA: Debug3: read %s string, SAtempreject is now changed to %f", SAgreylistiswhitestr, SAtemprejectthreshold);
		}
	    }
	    else
	    {
		if (SAEximDebug > 2)
		{
		    log_write(0, LOG_MAIN, "SA: Debug3: did not find read GREYLIST_ISWHITE string in X-Spam-Status");
		}
	    }

	    start=strstr(xspamstatus, "hits=");
	    /* Support SA 3.0 format */
	    if (start == NULL)
	    {
	    	start=strstr(xspamstatus, "score=");
	    }

	    end=strstr(xspamstatus, " tests=");
	    if (end == NULL)
	    {
		if (SAEximDebug > 5)
		{
		    log_write(0, LOG_MAIN, "SA: Debug6: Could not find old spamstatus format, trying new one...");
		}
		end=strstr(xspamstatus, "\n	tests=");
	    }
	    if (start!=NULL && end!=NULL)
	    {
		spamstatus=string_copyn(start, end-start);
		if (SAEximDebug > 2)
		{
		    log_write(0, LOG_MAIN, "SA: Debug3: Read from X-Spam-Status: %s", spamstatus);
		}
	    }
	    else
	    {
		PANIC(string_sprintf("SA: could not parse X-Spam-Status: to extract hits and required. Bad!. Got: '%s'", xspamstatus));
	    }

	    start=strstr(spamstatus, "=");
	    end=strstr(spamstatus, " ");
	    if (start!=NULL && end!=NULL)
	    {
		start++;
		sscanf(start, "%f", &spamvalue);
	    }
	    else
	    {
		PANIC(string_sprintf("SA: spam value extract failed in '%s'. Bad!", xspamstatus));
	    }
	
	    goto restart;
	}

	if (parsemlheader(buffer, (FILE *)readfh, "X-Spam-", NULL)) goto restart;

	/* Ok, now we can do normal processing */

	/* If no more headers here, we're done */
	if (buffer[0] == 0)
	{
	    if (SAEximDebug > 5)
	    {
		log_write(0, LOG_MAIN, "SA: Debug6: spamc read got newline, end of headers", buffer);
	    }
	    goto exit;
	}

	if (compare_header(buffer, "Message-Id: "))
	{
	    char *start;
	    char *end;
	    char *mesgid=NULL;

	    start=strchr(buffer, '<');
	    end=strchr(buffer, '>');

	    if (start == NULL || end == NULL)
	    {
	        /* we keep the default mesgfn (unix date in seconds) */
		if (SAEximDebug)
		{
		    log_write(0, LOG_MAIN, "SA: Debug: Could not get Message-Id from %s", buffer);
		}
	    }
	    else if ((mesgid=string_copyn(start+1,end-start-1)) && mesgid[0])
	    {
                /* We replace the exim Message-ID with the one read from
                the message * as we use this to detect dupes when we
                send 45x and get the same * message multiple times */
		safemesgid=cleanmsgid(mesgid, SAsafemesgidchars);
		mesgfn=string_sprintf("%d_%s", time(NULL), safemesgid);

		if (SAEximDebug > 5)
		{
		    log_write(0, LOG_MAIN, "SA: Debug6: Message-Id received and cleaned as: %s", safemesgid);
		}
	    }
	    continue;
	}
    }

    exit:


    if (isspam && SARewriteBody == 1)
    {
	int line;

	if (SAEximDebug)
	{
	    log_write(0, LOG_MAIN, "SA: Debug: SARewriteBody == 1, rewriting message body");
	}

	/* already read from fd? Better reset it... */
	ret=lseek(fd, fdstart, SEEK_SET);
	CHECKERR(ret, "lseek reset on spooled message", __LINE__);

	line=1;
	while ((fgets((char *)buffer,sizeof(buffera),(FILE *) readfh)) != NULL)
	{
	    if (SAEximDebug > 8)
	    {
		log_write(0, LOG_MAIN, "SA: Debug9: Read body from SA; line %d (read %d)", line, strlen(buffer));
	    }

	    stret=write(fd, buffer, strlen(buffer));
	    CHECKERR(stret,string_sprintf("%s", "SA body write to msg"),__LINE__);
	    if (SAEximDebug > 8)
	    {
		log_write(0, LOG_MAIN, "SA: Debug9: Wrote to msg; line %d (wrote %d)", line, ret);
	    }
	    if (buffer[strlen(buffer)-1] == '\n')
	    {
		line++;
	    }
	}

	
	if (SAEximDebug > 1)
	{
	    log_write(0, LOG_MAIN, "SA: Debug2: body_linecount before SA: %d", body_linecount);
	}

	/* update global variable $body_linecount to reflect the new body size*/
	if (body_linecount > 0) body_linecount = (line - 1); // Not updating if zero, indicating spool_wireformat

	if (SAEximDebug > 1)
	{
	    log_write(0, LOG_MAIN, "SA: Debug2: body_linecount after SA: %d", body_linecount);
	}

    }

    fclose((FILE *)readfh);

    afterscan=time(NULL);
    scantime=afterscan-beforescan;

    wait(&ret);
    signal(SIGCHLD, old_sigchld);

    if (ret)
    {
	sprintf(buffer, "%d", ret);
	PANIC(string_sprintf("wait on spamc child yielded, %s", buffer));
    }

    afterwait=time(NULL);
    fulltime=afterwait-beforescan;

    if(!SAaddSAEheaderBeforeSA)
    {
	AddSAEheaders((char *)rcptlist, SAmaxrcptlistlength);
    }

    header_add(' ', "X-SA-Exim-Version: %s\n",version);

    if (gotsa == 0)
    {
	header_add(' ', "X-SA-Exim-Scanned: No (on %s); Unknown failure\n", primary_hostname);
	log_write(0, LOG_MAIN, "SA: Action: SA didn't successfully run against message, accepting (time: %d/%d secs | Message-Id: %s). %s", scantime, fulltime, safemesgid, mailinfo);
	return LOCAL_SCAN_ACCEPT;
    }

    header_add(' ', "X-SA-Exim-Scanned: Yes (on %s)\n", primary_hostname);

    if (spamstatus == NULL)
    {
	spamstatus = (char *) nospamstatus;
    }
    if (isspam)
    {
	int dorej=1;
	int doteergrube=0;

	if (SAEximRejCond[0] != '1' || SAEximRejCond[1] != 0)
	{
	    expand=expand_string(SAEximRejCond);
	    if (expand == NULL)
	    {
		PANIC(string_sprintf("SAEximRejCond expansion failure on %s", SAEximRejCond));
	    }

	    if (SAEximDebug)
	    {
		log_write(0, LOG_MAIN, "SA: Debug: SAEximRejCond expand returned: '%s'", expand);
	    }

	    if (expand[0] == 0 || (expand[0] == '0' && expand[1] == 0))
	    {
		log_write(0, LOG_MAIN, "SA: Notice: SAEximRejCond expanded to false, not applying reject rules");
		dorej=0;
	    }
	}

	if (dorej && spamvalue >= SAteergrubethreshold)
	{
	    doteergrube=1;
	    if (SAteergrubecond[0] != '1' || SAteergrubecond[1] != 0)
	    {
		expand=expand_string(SAteergrubecond);
		if (expand == NULL)
		{
		    PANIC(string_sprintf("SAteergrubecond expansion failure on %s", SAteergrubecond));
		}

		if (SAEximDebug)
		{
		    log_write(0, LOG_MAIN, "SA: Debug: SAteergrubecond expand returned: '%s'", expand);
		}

		if (expand[0] == 0 || (expand[0] == '0' && expand[1] == 0))
		{
		    log_write(0, LOG_MAIN, "SA: Notice: SAteergrubecond expanded to false, not teergrubing known peer");
		    doteergrube=0;
		}
	    }
	}
	
	if (dorej && doteergrube)
	{
	    char *teergrubewaitstr;
	    teergrubewaitstr=string_sprintf(SAmsgteergrubewait, spamstatus);

	    /* By default, we'll only save temp bounces by message ID so
	     * that when the same message is submitted several times, we
	     * overwrite the same file on disk and not create a brand new
	     * one every single time */
	    if (SAteergrubeoverwrite)
	    {
		ret=savemail(fd, fdstart, SAteergrubesave, "SAteergrubesave", safemesgid, SAmaxarchivebody, SAteergrubeSavCond);
		CHECKERR(ret,where,line);
	    }
	    else
	    {
		ret=savemail(fd, fdstart, SAteergrubesave, "SAteergrubesave", (char *)mesgfn, SAmaxarchivebody, SAteergrubeSavCond);
		CHECKERR(ret,where,line);
	    }

	    spamstatus=string_sprintf("%s trigger=%.1f", spamstatus, SAteergrubethreshold);
	    /* Exim might want to stop us if we run for too long, but that's
	     * exactly what we're trying to do, so let's override that */
	    alarm(0);

	    for (i=0;i<SAteergrubetime/10;i++)
	    {
		smtp_printf("451-%s\r\n", teergrubewaitstr);
		ret=smtp_fflush();
		if (ret != 0)
		{
		    log_write(0, LOG_MAIN | LOG_REJECT, "SA: Action: teergrubed sender for %d secs until it closed the connection: %s (scanned in %d/%d secs | Message-Id: %s). %s", i*10, spamstatus, scantime, fulltime, safemesgid, mailinfo);
		    /* The other side closed the connection, nothing to print */
		    *return_text="";
		    return LOCAL_SCAN_TEMPREJECT_NOLOGHDR;
		}
		sleep(10);
	    }

	    log_write(0, LOG_MAIN | LOG_REJECT, "SA: Action: teergrubed sender until full configured duration of %d secs: %s (scanned in %d/%d secs | Message-Id: %s). %s", SAteergrubetime, spamstatus, scantime, fulltime, safemesgid, mailinfo);
	    *return_text=string_sprintf(SAmsgteergruberej, spamstatus);
	    return LOCAL_SCAN_TEMPREJECT_NOLOGHDR;
	}
	else if (dorej && spamvalue >= SAdevnullthreshold)
	{
	    ret=savemail(fd, fdstart, SAdevnullsave, "SAdevnullsave", (char *)mesgfn, SAmaxarchivebody, SAdevnullSavCond);
	    CHECKERR(ret,where,line);

	    recipients_count=0;
	    spamstatus=string_sprintf("%s trigger=%.1f", spamstatus, SAdevnullthreshold);
	    log_write(0, LOG_REJECT | LOG_MAIN, "SA: Action: silently tossed message: %s (scanned in %d/%d secs | Message-Id: %s). %s", spamstatus, scantime, fulltime, safemesgid, mailinfo);
	    return LOCAL_SCAN_ACCEPT;
	}
	else if (dorej && spamvalue >= SApermrejectthreshold)
	{
	    ret=savemail(fd, fdstart, SApermrejectsave, "SApermrejectsave", (char *)mesgfn, SAmaxarchivebody, SApermrejectSavCond);
	    CHECKERR(ret,where,line);

	    spamstatus=string_sprintf("%s trigger=%.1f", spamstatus, SApermrejectthreshold);
	    log_write(0, LOG_MAIN | LOG_REJECT, "SA: Action: permanently rejected message: %s (scanned in %d/%d secs | Message-Id: %s). %s", spamstatus, scantime, fulltime, safemesgid, mailinfo);
	    *return_text=string_sprintf(SAmsgpermrej, spamstatus);
	    return LOCAL_SCAN_REJECT_NOLOGHDR;
	}
	else if (dorej && spamvalue >= SAtemprejectthreshold)
	{
	    /* Yeah, gotos are harmful, but that'd be a function with a lot
	     * of options to send, so, here's a small shortcut */
	    goto dotempreject;
	}
	else
	{
	    ret=savemail(fd, fdstart, SAspamacceptsave, "SAspamacceptsave", (char *)mesgfn, SAmaxarchivebody, SAspamacceptSavCond);
	    CHECKERR(ret,where,line);
	    log_write(0, LOG_MAIN, "SA: Action: flagged as Spam but accepted: %s (scanned in %d/%d secs | Message-Id: %s). %s", spamstatus, scantime, fulltime, safemesgid, mailinfo);
	    return LOCAL_SCAN_ACCEPT;
	}
    }
    else
    {
	/* This is an exception to the rule, for grey listing, we allow for
	 * sending back a tempreject on SA scores that aren't considered as
	 * spam (greylisting is now done directly in spamassassin though */
	if (spamvalue >= SAtemprejectthreshold)
	{
	    dotempreject:

	    /* By default, we'll only save temp bounces by message ID so
	     * that when the same message is submitted several times, we
	     * overwrite the same file on disk and not create a brand new
	     * one every single time */
	    if (SAtemprejectoverwrite)
	    {
		ret=savemail(fd, fdstart, SAtemprejectsave, "SAtemprejectsave", safemesgid, SAmaxarchivebody, SAtemprejectSavCond);
		CHECKERR(ret,where,line);
	    }
	    else
	    {
		ret=savemail(fd, fdstart, SAtemprejectsave, "SAtemprejectsave", (char *)mesgfn, SAmaxarchivebody, SAtemprejectSavCond);
		CHECKERR(ret,where,line);
	    }

	    spamstatus=string_sprintf("%s trigger=%.1f", spamstatus, SAtemprejectthreshold);
	    log_write(0, LOG_MAIN | LOG_REJECT, "SA: Action: temporarily rejected message: %s (scanned in %d/%d secs | Message-Id: %s). %s", spamstatus, scantime, fulltime, safemesgid, mailinfo);
	    *return_text=string_sprintf(SAmsgtemprej, spamstatus);
	    return LOCAL_SCAN_TEMPREJECT_NOLOGHDR;
	}
	else
	{
	    ret=savemail(fd, fdstart, SAnotspamsave, "SAnotspamsave", (char *)mesgfn, SAmaxarchivebody, SAnotspamSavCond);
	    CHECKERR(ret,where,line);
	    log_write(0, LOG_MAIN, "SA: Action: scanned but message isn't spam: %s (scanned in %d/%d secs | Message-Id: %s). %s", spamstatus, scantime, fulltime, safemesgid, mailinfo);
	    return LOCAL_SCAN_ACCEPT;
	}
    }


	
    errexit:
    if (SAtemprejectonerror)
    {
	log_write(0, LOG_MAIN | LOG_PANIC, "SA: PANIC: Unexpected error on %s, file "__FILE__", line %d: %s", where, line-1, strerror(errno));
    }
    else
    {
	log_write(0, LOG_MAIN, "SA: PANIC: Unexpected error on %s (but message was accepted), file "__FILE__", line %d: %s", where, line-1, strerror(errno));
    }

    header_add(' ', "X-SA-Exim-Scanned: No (on %s); Exit with error (see exim mainlog)\n", primary_hostname);

    ret=savemail(fd, fdstart, SAerrorsave, "SAerrorsave", (char *)mesgfn, SAerrmaxarchivebody, SAerrorSavCond);
    if (ret < 0)
    {
	log_write(0, LOG_MAIN | LOG_PANIC, "SA: PANIC: Error in error handler while trying to save mail to %s, file "__FILE__", line %d: %s", string_sprintf("%s/%s", SAerrorsave, mesgfn), __LINE__ - 3, strerror(errno));
    }

    if (SAtemprejectonerror)
    {
	*return_text=SAmsgerror;
	return LOCAL_SCAN_TEMPREJECT_NOLOGHDR;
    }
    else
    {
	return LOCAL_SCAN_ACCEPT;
    }


    panicexit:
    if (SAtemprejectonerror)
    {
	log_write(0, LOG_MAIN | LOG_PANIC, "SA: PANIC: %s", panicerror);
    }
    else
    {
	log_write(0, LOG_MAIN | LOG_PANIC, "SA: PANIC: %s (but message was accepted)", panicerror);
    }

    header_add(' ', "X-SA-Exim-Scanned: No (on %s); Panic (see exim mainlog)\n", primary_hostname);

    ret=savemail(fd, fdstart, SAerrorsave, "SAerrorsave", (char *)mesgfn, SAerrmaxarchivebody, SAerrorSavCond);
    if (ret < 0)
    {
	log_write(0, LOG_MAIN | LOG_PANIC , "SA: PANIC: Error in error handler while trying to save mail to %s, file "__FILE__", line %d: %s", string_sprintf("%s/%s", SAerrorsave, mesgfn), __LINE__ - 3, strerror(errno));
    }

    if (SAtemprejectonerror)
    {
	*return_text=SAmsgerror;
	return LOCAL_SCAN_TEMPREJECT_NOLOGHDR;
    }
    else
    {
	return LOCAL_SCAN_ACCEPT;
    }
}
