/* BLURB gpl

                           Coda File System
                              Release 6

          Copyright (c) 1987-2003 Carnegie Mellon University
                  Additional copyrights listed below

This  code  is  distributed "AS IS" without warranty of any kind under
the terms of the GNU General Public Licence Version 2, as shown in the
file  LICENSE.  The  technical and financial  contributors to Coda are
listed in the file CREDITS.

                        Additional copyrights
                           none currently

#*/

/*
 *
 * Implementation of the Venus Communications subsystem.
 *
 *    This module should be split up into:
 *        1. A subsystem independent module, libcomm.a, containing base classes srvent and connent.
 *        2. A subsystem dependent module containing derived classes v_srvent and v_connent.
 *
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#include <sys/time.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include "coda_string.h"
#include <struct.h>
#include <unistd.h>
#include <stdlib.h>

#include <rpc2/rpc2.h>
#include <rpc2/se.h>
#include <rpc2/fail.h>
#include <rpc2/errors.h>

extern int Fcon_Init(); 
extern void SFTP_SetDefaults (SFTP_Initializer *initPtr);
extern void SFTP_Activate (SFTP_Initializer *initPtr);

/* interfaces */
#include <vice.h>

#ifdef __cplusplus
}
#endif

/* from vv */
#include <inconsist.h>

/* from venus */
#include "comm.h"
#include "fso.h"
#include "mariner.h"
#include "user.h"
#include "venus.private.h"
#include "venusrecov.h"
#include "venusvol.h"
#include "vproc.h"
#include "adv_monitor.h"
#include "adv_daemon.h"

int COPModes = 6;	/* ASYNCCOP2 | PIGGYCOP2 */
int UseMulticast = 0;
char myHostName[MAXHOSTNAMELEN];
int rpc2_retries = UNSET_RT;
int rpc2_timeout = UNSET_TO;
int sftp_windowsize = UNSET_WS;
int sftp_sendahead = UNSET_SA;
int sftp_ackpoint = UNSET_AP;
int sftp_packetsize = UNSET_PS;
int rpc2_timeflag = UNSET_ST;
int mrpc2_timeflag = UNSET_MT;
unsigned long WCThresh = UNSET_WCT;  	/* in Bytes/sec */
int WCStale = UNSET_WCS;	/* seconds */

extern long RPC2_Perror;
struct CommQueueStruct CommQueue;

#define MAXFILTERS 32

struct FailFilterInfoStruct {
	unsigned id;
	unsigned long host;
	FailFilterSide side;
	unsigned char used;
} FailFilterInfo[MAXFILTERS];

olist *srvent::srvtab;
char srvent::srvtab_sync;
olist *connent::conntab;
char connent::conntab_sync;

#ifdef	VENUSDEBUG
int connent::allocs = 0;
int connent::deallocs = 0;
int srvent::allocs = 0;
int srvent::deallocs = 0;
#endif /* VENUSDEBUG */


void CommInit() {
    /* Initialize unset command-line parameters. */
    if (rpc2_retries == UNSET_RT) rpc2_retries = DFLT_RT;
    if (rpc2_timeout == UNSET_TO) rpc2_timeout = DFLT_TO;
    if (sftp_windowsize == UNSET_WS) sftp_windowsize = DFLT_WS;
    if (sftp_sendahead == UNSET_SA) sftp_sendahead = DFLT_SA;
    if (sftp_ackpoint == UNSET_AP) sftp_ackpoint = DFLT_AP;
    if (sftp_packetsize == UNSET_PS) sftp_packetsize = DFLT_PS;
    if (rpc2_timeflag == UNSET_ST)
	srv_ElapseSwitch = DFLT_ST;
    else
	srv_ElapseSwitch = rpc2_timeflag;
    if (mrpc2_timeflag == UNSET_MT)
	srv_MultiStubWork[0].opengate = DFLT_MT;
    else
	srv_MultiStubWork[0].opengate = mrpc2_timeflag;

    if (WCThresh == UNSET_WCT) WCThresh = DFLT_WCT;
    if (WCStale == UNSET_WCS) WCStale = DFLT_WCS;

    /* Sanity check COPModes. */
    if ( (ASYNCCOP1 && !ASYNCCOP2) ||
	 (PIGGYCOP2 && !ASYNCCOP2) )
	CHOKE("CommInit: bogus COPModes (%x)", COPModes);

    /* Initialize comm queue */
    memset((void *)&CommQueue, 0, sizeof(CommQueueStruct));

    /* Hostname is needed for file server connections. */
    if (gethostname(myHostName, MAXHOSTNAMELEN) < 0)
	CHOKE("CommInit: gethostname failed");

    /* Initialize Connections. */
    connent::conntab = new olist;

    /* Initialize Servers. */
    srvent::srvtab = new olist;

    RPC2_Perror = 0;

    /* Port initialization. */
    RPC2_PortIdent port1;
    port1.Tag = RPC2_PORTBYINETNUMBER;
    port1.Value.InetPortNumber = htons(masquerade_port);

    /* SFTP initialization. */
    SFTP_Initializer sei;
    SFTP_SetDefaults(&sei);
    sei.WindowSize = sftp_windowsize;
    sei.SendAhead = sftp_sendahead;
    sei.AckPoint = sftp_ackpoint;
    sei.PacketSize = sftp_packetsize;
    sei.EnforceQuota = 1;
    sei.Port.Tag = (enum PortTag)0;
    SFTP_Activate(&sei);

    /* RPC2 initialization. */
    struct timeval tv;
    tv.tv_sec = rpc2_timeout;
    tv.tv_usec = 0;
    if (RPC2_Init(RPC2_VERSION, 0, &port1, rpc2_retries, &tv) != RPC2_SUCCESS)
	CHOKE("CommInit: RPC2_Init failed");

#ifdef USE_FAIL_FILTERS
    /* Failure package initialization. */
    memset((void *)FailFilterInfo, 0, (int) (MAXFILTERS * sizeof(struct FailFilterInfoStruct)));
    Fail_Initialize("venus", 0);
    Fcon_Init();
#endif

    /* Fire up the probe daemon. */
    PROD_Init();
}


/* *****  Connection  ***** */

int srvent::GetConn(connent **cpp, uid_t uid, int Force)
{
    LOG(100, ("srvent::GetConn: host = %s, uid = %d, force = %d\n",
              name, uid, Force));

    *cpp = 0;
    int code = 0;
    connent *c = 0;

    /* Grab an existing connection if one is free. */
    {
	/* Check whether there is already a free connection. */
	struct ConnKey Key; Key.host = host; Key.uid = uid;
	conn_iterator next(&Key);
	int count = 0;
	while ((c = next())) {
	    count++;
	    if (!c->inuse) {
		c->inuse = 1;
                *cpp = c;
                return 0;
	    }
	}
    }

    /* Try to connect to the server on behalf of the user. */
    RPC2_Handle ConnHandle = 0;
    int auth = 1;
    code = Connect(&ConnHandle, &auth, uid, Force);

    switch(code) {
    case 0:      break;
    case EINTR:  return(EINTR);
    case EPERM:
    case ERETRY: return(ERETRY);
    default:     return(ETIMEDOUT);
    }

    /* Create and install the new connent. */
    c = new connent(this, uid, ConnHandle, auth);
    if (!c) return(ENOMEM);

    c->inuse = 1;
    connent::conntab->insert(&c->tblhandle);
    *cpp = c;
    return(0);
}


void PutConn(connent **cpp)
{
    connent *c = *cpp;
    *cpp = 0;
    if (c == 0) {
	LOG(100, ("PutConn: null conn\n"));
	return;
    }

    LOG(100, ("PutConn: host = %s, uid = %d, cid = %d, auth = %d\n",
	      c->srv->Name(), c->uid, c->connid, c->authenticated));

    if (!c->inuse)
	{ c->print(logFile); CHOKE("PutConn: conn not in use"); }

    if (c->dying) {
	connent::conntab->remove(&c->tblhandle);
	delete c;
    }
    else {
	c->inuse = 0;
    }
}


void ConnPrint() {
    ConnPrint(stdout);
}


void ConnPrint(FILE *fp) {
    fflush(fp);
    ConnPrint(fileno(fp));
}


void ConnPrint(int fd) {
    if (connent::conntab == 0) return;

    fdprint(fd, "Connections: count = %d\n", connent::conntab->count());

    /* Iterate through the individual entries. */
    conn_iterator next;
    connent *c;
    while ((c = next())) c->print(fd);

    fdprint(fd, "\n");
}


connent::connent(srvent *server, uid_t Uid, RPC2_Handle cid, int authflag)
{
    LOG(1, ("connent::connent: host = %s, uid = %d, cid = %d, auth = %d\n",
	     server->Name(), uid, cid, authflag));

    /* These members are immutable. */
    server->GetRef();
    srv = server;
    uid = Uid;
    connid = cid;
    authenticated = authflag;

    /* These members are mutable. */
    inuse = 0;
    dying = 0;

#ifdef	VENUSDEBUG
    allocs++;
#endif
}


connent::~connent() {
#ifdef	VENUSDEBUG
    deallocs++;
#endif

    LOG(1, ("connent::~connent: host = %s, uid = %d, cid = %d, auth = %d\n",
	    srv->Name(), uid, connid, authenticated));

    int code = (int) RPC2_Unbind(connid);
    connid = 0;
    PutServer(&srv);
    LOG(1, ("connent::~connent: RPC2_Unbind -> %s\n", RPC2_ErrorMsg(code)));
}


int connent::Suicide(int disconnect)
{
    LOG(1, ("connent::Suicide: disconnect = %d\n", disconnect));

    /* Mark this conn as dying. */
    dying = 1;

    /* Can't do any more if it is busy. */
    if (inuse) return(0);

    inuse = 1;

    /* Be nice and disconnect if requested. */
    if (disconnect) {
	/* Make the RPC call. */
	MarinerLog("fetch::DisconnectFS %s\n", srv->Name());
	UNI_START_MESSAGE(ViceDisconnectFS_OP);
	int code = (int) ViceDisconnectFS(connid);
	UNI_END_MESSAGE(ViceDisconnectFS_OP);
	MarinerLog("fetch::disconnectfs done\n");
	code = CheckResult(code, 0);
	UNI_RECORD_STATS(ViceDisconnectFS_OP);
    }

    connent *myself = this; // needed because &this is illegal
    PutConn(&myself);

    return(1);
}


/* Maps return codes from Vice:
	0		Success,
	EINTR		Call was interrupted,
	ETIMEDOUT	Host did not respond,
	ERETRY		Retryable error,
	Other (> 0)	Non-retryable error (valid kernel return code).
*/
int connent::CheckResult(int code, VolumeId vid, int TranslateEINCOMP) {
    LOG(100, ("connent::CheckResult: code = %d, vid = %x\n",
	     code, vid));

    /* ViceOp succeeded. */
    if (code == 0) return(0);

    /* Translate RPC and Volume errors, and update server state. */
    switch(code) {
	default:
	    if (code < 0)
		srv->ServerError(&code);

	    if (code == ETIMEDOUT || code == ERETRY)
		dying = 1;
	    break;

	case VBUSY:
	    code = EWOULDBLOCK;
	    break;

	case VNOVOL:
	    code = ENXIO;
	    break;

	case VNOVNODE:
	    code = ENOENT;
	    break;

	case VLOGSTALE:
	    code = EALREADY;
	    break;

	case VSALVAGE:
	case VVOLEXISTS:
	case VNOSERVICE:
	case VOFFLINE:
	case VONLINE:
	case VNOSERVER:
	case VMOVED:
	case VFAIL:
	    eprint("connent::CheckResult: illegal code (%d)", code);
	    code = EINVAL;
	    break;
    }

    /* Coerce EINCOMPATIBLE to ERETRY. */
    if (TranslateEINCOMP && code == EINCOMPATIBLE)
        code = ERETRY;

    if (code == ETIMEDOUT && VprocInterrupted()) return(EINTR);
    return(code);
}


void connent::print(int fd) {
    fdprint(fd, "%#08x : host = %s, uid = %d, cid = %d, auth = %d, inuse = %d, dying = %d\n",
	     (long)this, srv->Name(), uid, connid, authenticated, inuse, dying);
}


conn_iterator::conn_iterator(struct ConnKey *Key) : olist_iterator((olist&)*connent::conntab) {
    key = Key;
}


connent *conn_iterator::operator()() {
    olink *o;
    while ((o = olist_iterator::operator()())) {
	connent *c = strbase(connent, o, tblhandle);
	if (key == (struct ConnKey *)0) return(c);
	if ((key->host.s_addr == c->srv->host.s_addr ||
             key->host.s_addr == INADDR_ANY) &&
	    (key->uid == c->uid || key->uid == ALL_UIDS))
	    return(c);
    }

    return(0);
}


/* ***** Server  ***** */

/*
 *    Notes on the srvent::connid field:
 *
 *    The server's connid is the "local handle" of the current callback connection.
 *
 *    A srvent::connid value of 0 serves as a flag that the server is incommunicado
 *    (i.,e., "down" from the point of view of this Venus).  Two other values are distinguished
 *    and mean that the server is "quasi-up": -1 indicates that the server has never been
 *    contacted (i.e., at initialization), -2 indicates that the server has just NAK'ed an RPC.
 */

#define	SRVRQ_LOCK()
#define	SRVRQ_UNLOCK()
#define	SRVRQ_WAIT()	    VprocWait((char *)&srvent::srvtab_sync)
#define	SRVRQ_SIGNAL()	    VprocSignal((char *)&srvent::srvtab_sync)

void Srvr_Wait() {
    SRVRQ_LOCK();
    LOG(0, ("WAITING(SRVRQ):\n"));
    START_TIMING();
    SRVRQ_WAIT();
    END_TIMING();
    LOG(0, ("WAIT OVER, elapsed = %3.1f\n", elapsed));
    SRVRQ_UNLOCK();
}


void Srvr_Signal() {
    SRVRQ_LOCK();
    LOG(10, ("SIGNALLING(SRVRQ):\n"));
    SRVRQ_SIGNAL();
    SRVRQ_UNLOCK();
}


srvent *FindServer(struct in_addr *host)
{
    srv_iterator next;
    srvent *s;

    while ((s = next()))
	if (s->host.s_addr == host->s_addr)
            return(s);

    return(0);
}


srvent *FindServerByCBCid(RPC2_Handle connid)
{
    if (connid == 0) return(0);

    srv_iterator next;
    srvent *s;

    while ((s = next()))
	if (s->connid == connid) return(s);

    return(0);
}


srvent *GetServer(struct in_addr *host, RealmId realm)
{
    CODA_ASSERT(host && host->s_addr);
    LOG(100, ("GetServer: host = %s\n", inet_ntoa(*host)));

    srvent *s = FindServer(host);
    if (s) {
        s->GetRef();
	return s;
    }

    s = new srvent(host, realm);

    srvent::srvtab->insert(&s->tblhandle);

    return s;
}


void PutServer(srvent **spp)
{
    if (*spp) {
	LOG(100, ("PutServer: %s\n", (*spp)->name));
        (*spp)->PutRef();
    }
    *spp = NULL;
}


/*
 *    The probe routines exploit parallelism in three ways:
 *       1. MultiRPC is used to perform the Probe RPC (actually, a ViceGetTime)
 *       2. Slave vprocs are used to overlap the probing of "up" servers and 
 * 	    the binding/probing of "down" servers.  Otherwise probing of "up"
 *	    servers may be delayed for the binding to "down" servers.
 *       3. (Additional) slave vprocs are used to overlap the binding of 
 *          "down" servers
 *
 *    Note that item 3 is only needed because MultiBind functionality is not 
 *    yet a part of MultiRPC.
 */

probeslave::probeslave(ProbeSlaveTask Task, void *Arg, void *Result, char *Sync) :
	vproc("ProbeSlave", NULL, VPT_ProbeDaemon, 16384) {
    LOG(100, ("probeslave::probeslave(%#x): %-16s : lwpid = %d\n", this, name, lwpid));

    task = Task;
    arg = Arg;
    result = Result;
    sync = Sync;

    /* Poke main procedure. */
    start_thread();
}

void probeslave::main(void)
{
    switch(task) {
	case ProbeUpServers:
	    ProbeServers(1);
	    break;

	case ProbeDownServers:
	    ProbeServers(0);
	    break;

	case BindToServer:
	    {
	    /* *result gets pointer to connent on success, 0 on failure. */
	    struct in_addr *Host = (struct in_addr *)arg;
            srvent *s = FindServer(Host);
	    if (s) {
		s->GetRef();
		s->GetConn((connent **)result, V_UID, 1);
		PutServer(&s);
	    }
	    }
	    break;

	default:
	    CHOKE("probeslave::main: bogus task (%d)", task);
    }

    /* Signal reaper, then commit suicide. */
    (*sync)++;
    VprocSignal(sync);
    idle = 1;
    delete VprocSelf();
}

void ProbeServers(int Up)
{
    LOG(1, ("ProbeServers: %s\n", Up ? "Up" : "Down"));

    /* Hosts and Connections are arrays of addresses and connents respectively
     * representing the servers to be probed.  HowMany is the current size of
     * these arrays, and ix is the number of entries actually used. */
    const int GrowSize = 32;
    int HowMany = GrowSize;
    struct in_addr *Hosts;
    int ix = 0;

    Hosts = (struct in_addr *)malloc(HowMany * sizeof(struct in_addr));
    /* Fill in the Hosts array for each server that is to be probed. */
    {
	srv_iterator next;
	srvent *s;
	while ((s = next())) {
	    if (!s->probeme)
		continue;

	    if ((Up && s->ServerIsDown()) || (!Up && !s->ServerIsDown()))
		continue;

	    /* Grow the Hosts array if necessary. */
	    if (ix == HowMany) {
		HowMany += GrowSize;
		Hosts = (struct in_addr *)
		    realloc(Hosts, HowMany * sizeof(struct in_addr));
		memset(&Hosts[ix], 0, GrowSize * sizeof(struct in_addr));
	    }

	    /* Stuff the address in the Hosts array. */
	    memcpy(&Hosts[ix], &s->host, sizeof(struct in_addr));
	    ix++;
	}
    }

    if (ix)
	DoProbes(ix, Hosts);

    /* the incorrect "free" in DoProbes() is moved here */
    free(Hosts);
}


void DoProbes(int HowMany, struct in_addr *Hosts)
{
    connent **Connections = 0;
    int i;

    CODA_ASSERT(HowMany > 0);

    Connections = (connent **)malloc(HowMany * sizeof(connent *));
    memset(Connections, 0, HowMany * sizeof(connent *));

    /* Bind to the servers. */
    MultiBind(HowMany, Hosts, Connections);

    /* Probe them. */
    int AnyHandlesValid = 0;
    RPC2_Handle *Handles = (RPC2_Handle *)malloc(HowMany * sizeof(RPC2_Handle));
    for (i = 0; i < HowMany; i++) {
	if (Connections[i] == 0) { Handles[i] = 0; continue; }

	AnyHandlesValid = 1;
	Handles[i] = Connections[i]->connid;
    }

    if (AnyHandlesValid)
	MultiProbe(HowMany, Handles);

    free(Handles);

    for (i = 0; i < HowMany; i++)
	PutConn(&Connections[i]);
    free(Connections);
}


void MultiBind(int HowMany, struct in_addr *Hosts, connent **Connections)
{
    if (LogLevel >= 1) {
	dprint("MultiBind: HowMany = %d\n\tHosts = [ ", HowMany);
	for (int i = 0; i < HowMany; i++)
	    fprintf(logFile, "%s ", inet_ntoa(Hosts[i]));
	fprintf(logFile, "]\n");
    }

    int ix, slaves = 0;
    char slave_sync = 0;
    for (ix = 0; ix < HowMany; ix++) {
	/* Try to get a connection without forcing a bind. */
	connent *c = 0;
	int code;
	srvent *s = FindServer(&Hosts[ix]);

	if (!s) continue;

	s->GetRef();
	code = s->GetConn(&c, V_UID);
	PutServer(&s);

	if (code == 0) {
	    /* Stuff the connection in the array. */
	    Connections[ix] = c;
	    continue;
	}

	/* Force a bind, but have a slave do it so we can bind in parallel. */
	{
	    slaves++;
	    (void)new probeslave(BindToServer, (void *)(&Hosts[ix]),
				 (void *)(&Connections[ix]), &slave_sync);
	}
    }

    /* Reap any slaves we created. */
    while (slave_sync != slaves) {
	LOG(1, ("MultiBind: waiting (%d, %d)\n", slave_sync, slaves));
	VprocWait(&slave_sync);
    }
}


void MultiProbe(int HowMany, RPC2_Handle *Handles)
{
    if (LogLevel >= 1) {
	dprint("MultiProbe: HowMany = %d\n\tHandles = [ ", HowMany);
	for (int i = 0; i < HowMany; i++)
	    fprintf(logFile, "%lx ", Handles[i]);
	fprintf(logFile, "]\n");
    }

    /* Make multiple copies of the IN/OUT and OUT parameters. */
    RPC2_Unsigned  **secs_ptrs =
	(RPC2_Unsigned **)malloc(HowMany * sizeof(RPC2_Unsigned *));
    CODA_ASSERT(secs_ptrs);
    RPC2_Unsigned   *secs_bufs =
	(RPC2_Unsigned *)malloc(HowMany * sizeof(RPC2_Unsigned));
    CODA_ASSERT(secs_bufs);
    for (int i = 0; i < HowMany; i++)
	secs_ptrs[i] = &secs_bufs[i]; 
    RPC2_Integer  **usecs_ptrs =
	(RPC2_Integer **)malloc(HowMany * sizeof(RPC2_Integer *));
    CODA_ASSERT(usecs_ptrs);
    RPC2_Integer   *usecs_bufs =
	(RPC2_Integer *)malloc(HowMany * sizeof(RPC2_Integer));
    CODA_ASSERT(usecs_bufs);
    for (int ii = 0; ii < HowMany; ii++)
	usecs_ptrs[ii] = &usecs_bufs[ii]; 

    /* Make the RPC call. */
    MarinerLog("fetch::Probe\n");
    MULTI_START_MESSAGE(ViceGetTime_OP);
    int code = (int) MRPC_MakeMulti(ViceGetTime_OP, ViceGetTime_PTR,
			       HowMany, Handles, (RPC2_Integer *)0, 0,
			       HandleProbe, 0, secs_ptrs, usecs_ptrs);
    MULTI_END_MESSAGE(ViceGetTime_OP);
    MarinerLog("fetch::probe done\n");

    /* CheckResult is done dynamically by HandleProbe(). */
    MULTI_RECORD_STATS(ViceGetTime_OP);

    /* Discard dynamic data structures. */
    free(secs_ptrs);
    free(secs_bufs);
    free(usecs_ptrs);
    free(usecs_bufs);
}


long HandleProbe(int HowMany, RPC2_Handle Handles[], long offset, long rpcval, ...)
{
    RPC2_Handle RPCid = Handles[offset];

    if (RPCid != 0) {
	/* Get the {host,port} pair for this call. */
	RPC2_PeerInfo thePeer;
	long rc = RPC2_GetPeerInfo(RPCid, &thePeer);
	if (thePeer.RemoteHost.Tag != RPC2_HOSTBYINETADDR ||
	    thePeer.RemotePort.Tag != RPC2_PORTBYINETNUMBER) {
	    LOG(0, ("HandleProbe: RPC2_GetPeerInfo return code = %d\n", rc));
	    LOG(0, ("HandleProbe: thePeer.RemoteHost.Tag = %d\n", thePeer.RemoteHost.Tag));
	    LOG(0, ("HandleProbe: thePeer.RemotePort.Tag = %d\n", thePeer.RemotePort.Tag));
	    return 0;
	    /* CHOKE("HandleProbe: getpeerinfo returned bogus type!"); */
	}

	/* Locate the server and update its status. */
	srvent *s = FindServer(&thePeer.RemoteHost.Value.InetAddress);
	if (!s)
	    CHOKE("HandleProbe: no srvent (RPCid = %d, PeerHost = %s)",
                  RPCid, inet_ntoa(thePeer.RemoteHost.Value.InetAddress));
	LOG(1, ("HandleProbe: (%s, %d)\n", s->name, rpcval));
	if (rpcval < 0)
	    s->ServerError((int *)&rpcval);
    }

    return(0);
}


/* Report which servers are down. */
void DownServers(char *buf, unsigned int *bufsize)
{
    char *cp = buf;
    unsigned int maxsize = *bufsize;
    *bufsize = 0;

    /* Copy each down server's address into the buffer. */
    srv_iterator next;
    srvent *s;
    while ((s = next()))
	if (s->ServerIsDown()) {
	    /* Make sure there is room in the buffer for this entry. */
	    if ((cp - buf) + sizeof(struct in_addr) > maxsize) return;

	    memcpy(cp, &s->host, sizeof(struct in_addr));
	    cp += sizeof(struct in_addr);
	}

    /* Null terminate the list.  Make sure there is room in the buffer for the
     * terminator. */
    if ((cp - buf) + sizeof(struct in_addr) > maxsize) return;
    memset(cp, 0, sizeof(struct in_addr));
    cp += sizeof(struct in_addr);

    *bufsize = (cp - buf);
}


/* Report which of a given set of servers is down. */
void DownServers(int nservers, struct in_addr *hostids,
                 char *buf, unsigned int *bufsize)
{
    char *cp = buf;
    unsigned int maxsize = *bufsize;
    *bufsize = 0;

    /* Copy each down server's address into the buffer. */
    for (int i = 0; i < nservers; i++) {
	srvent *s = FindServer(&hostids[i]);
	if (s && s->ServerIsDown()) {
	    /* Make sure there is room in the buffer for this entry. */
	    if ((cp - buf) + sizeof(struct in_addr) > maxsize) return;

	    memcpy(cp, &s->host, sizeof(struct in_addr));
	    cp += sizeof(struct in_addr);
	}
    }

    /* Null terminate the list.  Make sure there is room in the buffer for the
     * terminator. */
    if ((cp - buf) + sizeof(struct in_addr) > maxsize) return;
    memset(cp, 0, sizeof(struct in_addr));
    cp += sizeof(struct in_addr);

    *bufsize = (cp - buf);
}


/* 
 * Update bandwidth estimates for all up servers.
 * Reset estimates and declare connectivity strong if there are
 * no recent observations.  Called by the probe daemon.
 */
void CheckServerBW(long curr_time)
{
    srv_iterator next;
    srvent *s;
    unsigned long bw = INIT_BW;

    while ((s = next())) {
	if (s->ServerIsUp()) 
	    (void) s->GetBandwidth(&bw);
    }
}


void ServerPrint() {
    ServerPrint(stdout);
}

void ServerPrint(FILE *f)
{
    if (srvent::srvtab == 0) return;

    fprintf(f, "Servers: count = %d\n", srvent::srvtab->count());

    srv_iterator next;
    srvent *s;
    while ((s = next())) s->print(f);

    fprintf(f, "\n");
}


srvent::srvent(struct in_addr *Host, RealmId realm)
{
    LOG(1, ("srvent::srvent: host = %s\n", inet_ntoa(*Host)));

    struct hostent *h = gethostbyaddr((char *)Host, sizeof(struct in_addr), AF_INET);
    if (h) {
	name = new char[strlen(h->h_name) + 1];
	strcpy(name, h->h_name);
	TRANSLATE_TO_LOWER(name);
    }
    else {
	name = new char[16];
	sprintf(name, "%s", inet_ntoa(*Host));
    }

    host = *Host;
    realmid = realm;
    connid = -1;
    Xbinding = 0;
    probeme = 0;
    forcestrong = 0;
    isweak = 0;
    bw     = INIT_BW;
    bwmax  = INIT_BW;
    lastobs.tv_sec = lastobs.tv_usec = 0;
    VGAPlusSHA_Supported = 0;  /* default is old-style server */
    

#ifdef	VENUSDEBUG
    allocs++;
#endif
}

srvent::~srvent()
{
#ifdef	VENUSDEBUG
    deallocs++;
#endif

    LOG(1, ("srvent::~srvent: host = %s, conn = %d\n", name, connid));

    srvent::srvtab->remove(&tblhandle);

    delete [] name;

    int code = (int) RPC2_Unbind(connid);
    LOG(1, ("srvent::~srvent: RPC2_Unbind -> %s\n", RPC2_ErrorMsg(code)));
}


int srvent::Connect(RPC2_Handle *cidp, int *authp, uid_t uid, int Force)
{
    LOG(100, ("srvent::Connect: host = %s, uid = %d, force = %d\n",
	     name, uid, Force));

    int code = 0;

    /* See whether this server is down or already binding. */
    for (;;) {
	if (ServerIsDown() && !Force) {
	    LOG(100, ("srvent::Connect: server (%s) is down\n", name));
	    return(ETIMEDOUT);
	}

	if (!Xbinding) break;
	if (VprocInterrupted()) return(EINTR);
	Srvr_Wait();
	if (VprocInterrupted()) return(EINTR);
    }

    /* Get the user entry and attempt to connect to it. */
    Xbinding = 1;
    {
	userent *u = 0;
	Realm *realm = REALMDB->GetRealm(realmid);
	GetUser(&u, realm, uid);
	code = u->Connect(cidp, authp, &host);
	PutUser(&u);
	realm->PutRef();
    }
    Xbinding = 0;
    Srvr_Signal();

    /* Interpret result. */
    if (code < 0)
	switch (code) {
	    case RPC2_NOTAUTHENTICATED:
		code = EPERM; break;

	    case RPC2_NOBINDING:
	    case RPC2_SEFAIL2:
	    case RPC2_FAIL:
		code = ETIMEDOUT; break;

	    default:
/*
		CHOKE("srvent::Connect: illegal RPC code (%s)", RPC2_ErrorMsg(code));
*/
		code = ETIMEDOUT; break;
	}
    if (!ServerIsDown() && code == ETIMEDOUT) {
	/* Not already considered down. */
	MarinerLog("connection::unreachable %s\n", name);
	Reset();
	adv_mon.ServerInaccessible(name);
    }

    if (code == ETIMEDOUT && VprocInterrupted()) return(EINTR);
    return(code);
}

int srvent::GetStatistics(ViceStatistics *Stats)
{
    LOG(100, ("srvent::GetStatistics: host = %s\n", name));

    int code = 0;
    connent *c = 0;

    memset(Stats, 0, sizeof(ViceStatistics));
    
    code = GetConn(&c, V_UID);
    if (code != 0) goto Exit;

    /* Make the RPC call. */
    MarinerLog("fetch::GetStatistics %s\n", name);
    UNI_START_MESSAGE(ViceGetStatistics_OP);
    code = (int) ViceGetStatistics(c->connid, Stats);
    UNI_END_MESSAGE(ViceGetStatistics_OP);
    MarinerLog("fetch::getstatistics done\n");
    code = c->CheckResult(code, 0);
    UNI_RECORD_STATS(ViceGetStatistics_OP);

Exit:
    PutConn(&c);
    return(code);
}


void srvent::Reset()
{
    LOG(1, ("srvent::Reset: host = %s\n", name));

    /* Kill all direct connections to this server. */
    {
	struct ConnKey Key; Key.host = host; Key.uid = ALL_UIDS;
	conn_iterator conn_next(&Key);
	connent *c = 0;
	connent *tc = 0;
	for (c = conn_next(); c != 0; c = tc) {
	    tc = conn_next();		/* read ahead */
	    (void)c->Suicide(0);
	}
    }

    /* Unbind callback connection for this server. */
    int code = (int) RPC2_Unbind(connid);
    LOG(1, ("srvent::Reset: RPC2_Unbind -> %s\n", RPC2_ErrorMsg(code)));
    connid = 0;

    /* Send a downevent to volumes associated with this server */
    /* Also kills all indirect connections to the server. */
    VDB->DownEvent(&host);
}


void srvent::ServerError(int *codep)
{
    LOG(1, ("srvent::ServerError: %s error (%s)\n",
	    name, RPC2_ErrorMsg(*codep)));

    /* Translate the return code. */
    switch (*codep) {
	case RPC2_FAIL:
	case RPC2_NOCONNECTION:
	case RPC2_TIMEOUT:
	case RPC2_DEAD:
	case RPC2_SEFAIL2:
	    *codep = ETIMEDOUT; break;

	case RPC2_SEFAIL1:
	case RPC2_SEFAIL3:
	case RPC2_SEFAIL4:
	    *codep = EIO; break;

	case RPC2_NAKED:
	case RPC2_NOTCLIENT:
	    *codep = ERETRY; break;

        case RPC2_INVALIDOPCODE:
	    *codep = EOPNOTSUPP; break;

	default:
	    /* Map RPC2 warnings into EINVAL. */
	    if (*codep > RPC2_ELIMIT) { *codep = EINVAL; break; }
	    CHOKE("srvent::ServerError: illegal RPC code (%d)", *codep);
    }

    if (!ServerIsDown()) {
	/* Reset if TIMED'out or NAK'ed. */
	switch (*codep) {
	    case ETIMEDOUT:
	        MarinerLog("connection::unreachable %s\n", name);
		Reset();
		adv_mon.ServerInaccessible(name);
		break;

	    case ERETRY:
		/* Must have missed a down event! */
		eprint("%s nak'ed", name);
		Reset();
		connid = -2;
		VDB->UpEvent(&host);
		break;

	    default:
		break;
	}
    }
}


void srvent::ServerUp(RPC2_Handle newconnid)
{
    LOG(1, ("srvent::ServerUp: %s, connid = %d, newconnid = %d\n",
	     name, connid, newconnid));

    switch(connid) {
    case 0:
	MarinerLog("connection::up %s\n", name);
	connid = newconnid;
	VDB->UpEvent(&host);
	adv_mon.ServerAccessible(name);
        break;

    case -1:
	/* Initial case.  */
	connid = newconnid;
	VDB->UpEvent(&host);
        break;

    case -2:
	/* Following NAK.  Don't signal another UpEvent! */
	connid = newconnid;
        break;

    default:
	/* Already considered up.  Must have missed a down event! */
	Reset();
	connid = newconnid;
	VDB->UpEvent(&host);
    }

    /* Poke any threads waiting for a change in communication state. */
    Rtry_Signal();
}


long srvent::GetLiveness(struct timeval *tp)
{
    long rc = 0;
    struct timeval t;

    LOG(100, ("srvent::GetLiveness (%s)\n", name));

    tp->tv_sec = tp->tv_usec = 0;
    t.tv_sec = t.tv_usec = 0;

    /* we don't have a real connid if the server is down or "quasi-up" */
    if (connid <= 0) 
	return(ETIMEDOUT);

    /* Our peer is at the other end of the callback connection */
    if ((rc = RPC2_GetPeerLiveness(connid, tp, &t)) != RPC2_SUCCESS)
	return(rc);

    LOG(100, ("srvent::GetLiveness: (%s), RPC %ld.%0ld, SE %ld.%0ld\n",
	      name, tp->tv_sec, tp->tv_usec, t.tv_sec, t.tv_usec));

    if (tp->tv_sec < t.tv_sec ||
	(tp->tv_sec == t.tv_sec && tp->tv_usec < t.tv_usec))
	*tp = t;	/* structure assignment */

    return(0);
}


/* 
 * calculates current bandwidth to server, taking the current estimates from
 * RPC2/SFTP.
 *
 * Triggers weakly/strongly connected transitions if appropriate.
 */

/* returns bandwidth in Bytes/sec, or INIT_BW if it couldn't be obtained */
long srvent::GetBandwidth(unsigned long *Bandwidth)
{
    long rc = 0;
    unsigned long oldbw = bw;
    unsigned long bwmin;

    LOG(1, ("srvent::GetBandwidth (%s) lastobs %ld.%06ld\n", 
	      name, lastobs.tv_sec, lastobs.tv_usec));
    
    /* we don't have a real connid if the server is down or "quasi-up" */
    if (connid <= 0) 
	return(ETIMEDOUT);
    
    /* retrieve the bandwidth information from RPC2 */
    if ((rc = RPC2_GetBandwidth(connid, &bwmin, &bw, &bwmax)) != RPC2_SUCCESS)
	return(rc);

    LOG(1, ("srvent:GetBandWidth: --> new BW %d bytes/sec\n", bw));

    /* update last observation time */
    RPC2_GetLastObs(connid, &lastobs);

    /* 
     * Signal if we've crossed the weakly-connected threshold. Note
     * that the connection is considered strong until proven otherwise.
     *
     * The user can block the strong->weak transition using the
     * 'cfs strong' command (and turn adaptive mode back on with
     * 'cfs adaptive').
     */
    if (!isweak && !forcestrong && bwmax < WCThresh) {
	isweak = 1;
	MarinerLog("connection::weak %s\n", name);
	VDB->WeakEvent(&host);
        adv_mon.ServerConnectionWeak(name);
    }
    else if (isweak && bwmin > WCThresh) {
	isweak = 0;
	MarinerLog("connection::strong %s\n", name);
	VDB->StrongEvent(&host);
        adv_mon.ServerConnectionStrong(name);
    }
	
    *Bandwidth = bw;
    if (bw != oldbw) {
	MarinerLog("connection::bandwidth %s %d %d %d\n", name,bwmin,bw,bwmax);
        adv_mon.ServerBandwidthEstimate(name, *Bandwidth);
    }
    LOG(1, ("srvent::GetBandwidth (%s) returns %d bytes/sec\n",
	      name, *Bandwidth));
    return(0);
}


/* 
 * Force server connectivity to strong, or resume with the normal bandwidth
 * adaptive mode (depending on the `on' flag).
 */
void srvent::ForceStrong(int on) {
    forcestrong = on;

    /* forced switch to strong mode */
    if (forcestrong && isweak) {
	isweak = 0;
	MarinerLog("connection::strong %s\n", name);
	VDB->StrongEvent(&host);
        adv_mon.ServerConnectionStrong(name);
    }

    /* switch back to adaptive mode */
    if (!forcestrong && !isweak && bwmax < WCThresh) {
	isweak = 1;
	MarinerLog("connection::weak %s\n", name);
	VDB->WeakEvent(&host);
        adv_mon.ServerConnectionWeak(name);
    }

    return;
}


void srvent::print(FILE *f)
{
    fprintf(f, "%p : %-16s : cid = %d, host = %s, binding = %d, bw = %ld\n",
            this, name, (int)connid, inet_ntoa(host), Xbinding, bw);
    PrintRef(f);
}


srv_iterator::srv_iterator() : olist_iterator((olist&)*srvent::srvtab) {
}


srvent *srv_iterator::operator()() {
    olink *o = olist_iterator::operator()();
    if (!o) return(0);

    srvent *s = strbase(srvent, o, tblhandle);
    return(s);
}


#ifndef USE_FAIL_FILTERS
extern int (*Fail_SendPredicate)(unsigned char ip1, unsigned char ip2,
				 unsigned char ip3, unsigned char ip4,
				 unsigned char color, RPC2_PacketBuffer *pb,
				 struct sockaddr_in *sin, int fd);
extern int (*Fail_RecvPredicate)(unsigned char ip1, unsigned char ip2,
				 unsigned char ip3, unsigned char ip4,
				 unsigned char color, RPC2_PacketBuffer *pb,
				 struct sockaddr_in *sin, int fd);

static int DropPacket(unsigned char ip1, unsigned char ip2,
		      unsigned char ip3, unsigned char ip4,
		      unsigned char color, RPC2_PacketBuffer *pb,
		      struct sockaddr_in *sin, int fd)
{
    /* Tell rpc2 to drop the packet */
    return 0;
}

int FailDisconnect(int nservers, struct in_addr *hostids)
{
    Fail_SendPredicate = Fail_RecvPredicate = &DropPacket;
    return 0;
}

int FailReconnect(int nservers, struct in_addr *hostids)
{
    Fail_SendPredicate = Fail_RecvPredicate = NULL;
    return 0;
}

int FailSlow(unsigned *speedp)
{
    return -1;
}
#else
/* *****  Fail library manipulations ***** */

/* 
 * Simulate "pulling the plug". Insert filters on the
 * send and receive sides of venus.
 */
int FailDisconnect(int nservers, struct in_addr *hostids)
{
    int rc, k = 0;
    FailFilter filter;
    FailFilterSide side;

    do {
	srv_iterator next;
	srvent *s;
	while ((s = next()))
	    if (nservers == 0 || s->host.s_addr == hostids[k].s_addr) {
		/* we want a pair of filters for server s. */

		struct in_addr addr = s->host;    
		filter.ip1 = ((unsigned char *)&addr)[0];
		filter.ip2 = ((unsigned char *)&addr)[1];
		filter.ip3 = ((unsigned char *)&addr)[2];
		filter.ip4 = ((unsigned char *)&addr)[3];
		filter.color = -1;
		filter.lenmin = 0;
		filter.lenmax = 65535;
		filter.factor = 0;
		filter.speed = 0;
		filter.latency = 0;

		for (int i = 0; i < 2; i++) {
		    if (i == 0) side = sendSide;
		    else side = recvSide;

		    /* 
		     * do we have this filter already?  Note that this only
		     * checks the filters inserted from Venus.  To check all
		     * the filters, we need to use Fail_GetFilters.
		     */
		    char gotit = 0;
		    for (int j = 0; j < MAXFILTERS; j++) 
			if (FailFilterInfo[j].used && 
			    htonl(FailFilterInfo[j].host) == s->host.s_addr &&
			    FailFilterInfo[j].side == side) {
				gotit = 1;
				break;
			}

		    if (!gotit) {  /* insert a new filter */
			int ix = -1;    
			for (int j = 0; j < MAXFILTERS; j++) 
			    if (!FailFilterInfo[j].used) {
				ix = j;
				break;
			    }

			if (ix == -1) { /* no room */
			    LOG(0, ("FailDisconnect: couldn't insert %s filter for %s, table full!\n", 
				(side == recvSide)?"recv":"send", s->name));
			    return(ENOBUFS);
			}

			if ((rc = Fail_InsertFilter(side, 0, &filter)) < 0) {
			    LOG(0, ("FailDisconnect: couldn't insert %s filter for %s\n", 
				(side == recvSide)?"recv":"send", s->name));
			    return(rc);
			}
			LOG(10, ("FailDisconnect: inserted %s filter for %s, id = %d\n", 
			    (side == recvSide)?"recv":"send", s->name, rc));

			FailFilterInfo[ix].id = filter.id;
			FailFilterInfo[ix].side = side;
			FailFilterInfo[ix].host = ntohl(s->host.s_addr);
			FailFilterInfo[ix].used = 1;
		    }
		} 
	    }

    } while (k++ < nservers-1);

    return(0);
}

/* 
 * Remove fail filters inserted by VIOC_SLOW or VIOC_DISCONNECT.
 * Filters inserted by other applications (ttyfcon, etc) are not
 * affected, since we don't have their IDs. If there is a problem 
 * removing a filter, we print a message in the log and forget 
 * about it. This allows the user to remove the filter using another
 * tool and not have to deal with leftover state in venus.
 */
int FailReconnect(int nservers, struct in_addr *hostids)
{
    int rc, s = 0;

    do {
	for (int i = 0; i < MAXFILTERS; i++) 
	    if (FailFilterInfo[i].used &&
                (nservers == 0 ||
                 (htonl(FailFilterInfo[i].host) == hostids[s].s_addr))) {
                if ((rc = Fail_RemoveFilter(FailFilterInfo[i].side,
                                            FailFilterInfo[i].id)) < 0) {
		    LOG(0, ("FailReconnect: couldn't remove %s filter, id = %d\n", 
			(FailFilterInfo[i].side == sendSide)?"send":"recv", 
			FailFilterInfo[i].id));
		} else {
		    LOG(10, ("FailReconnect: removed %s filter, id = %d\n", 
			(FailFilterInfo[i].side == sendSide)?"send":"recv", 
			FailFilterInfo[i].id));
		    FailFilterInfo[i].used = 0;
		}
            }
    } while (s++ < nservers-1);

    return(0);
}

/* 
 * Simulate a slow network. Insert filters with the
 * specified speed to all known servers.
 */
int FailSlow(unsigned *speedp) {

    return(-1);
}
#endif

