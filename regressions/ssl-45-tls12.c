//
//  ssl-43-ciphers.c
//  regressions
//
//  Created by Fabrice Gautier on 6/7/11.
//  Copyright 2011 Apple, Inc. All rights reserved.
//

#include <stdbool.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <CoreFoundation/CoreFoundation.h>

#include <AssertMacros.h>
#include <Security/SecureTransportPriv.h> /* SSLSetOption */
#include <Security/SecureTransport.h>
#include <Security/SecPolicy.h>
#include <Security/SecTrust.h>
#include <Security/SecIdentity.h>
#include <Security/SecIdentityPriv.h>
#include <Security/SecCertificatePriv.h>
#include <Security/SecKeyPriv.h>
#include <Security/SecItem.h>
#include <Security/SecRandom.h>

#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdlib.h>
#include <mach/mach_time.h>

#include "ssl_regressions.h"

typedef struct {
    uint32_t session_id;
    bool is_session_resume;
    SSLContextRef st;
    bool is_server;
    bool client_side_auth;
    bool dh_anonymous;
    int comm;
    CFArrayRef certs;
} ssl_test_handle;



#if 0
static void hexdump(const uint8_t *bytes, size_t len) {
	size_t ix;
    printf("socket write(%p, %lu)\n", bytes, len);
	for (ix = 0; ix < len; ++ix) {
        if (!(ix % 16))
            printf("\n");
		printf("%02X ", bytes[ix]);
	}
	printf("\n");
}
#else
#define hexdump(bytes, len)
#endif

static int SocketConnect(const char *hostName, int port)
{
    struct sockaddr_in  addr;
    struct in_addr      host;
	int					sock;
    int                 err;
    struct hostent      *ent;

    if (hostName[0] >= '0' && hostName[0] <= '9')
    {
        host.s_addr = inet_addr(hostName);
    }
    else {
		unsigned dex;
#define GETHOST_RETRIES 5
		/* seeing a lot of soft failures here that I really don't want to track down */
		for(dex=0; dex<GETHOST_RETRIES; dex++) {
			if(dex != 0) {
				printf("\n...retrying gethostbyname(%s)", hostName);
			}
			ent = gethostbyname(hostName);
			if(ent != NULL) {
				break;
			}
		}
        if(ent == NULL) {
			printf("\n***gethostbyname(%s) returned: %s\n", hostName, hstrerror(h_errno));
            return -1;
        }
        memcpy(&host, ent->h_addr, sizeof(struct in_addr));
    }


    sock = socket(AF_INET, SOCK_STREAM, 0);
    addr.sin_addr = host;
    addr.sin_port = htons((u_short)port);

    addr.sin_family = AF_INET;
    err = connect(sock, (struct sockaddr *) &addr, sizeof(struct sockaddr_in));

    if(err!=0)
    {
        perror("connect failed");
        return err;
    }

    return sock;
}


static OSStatus SocketWrite(SSLConnectionRef conn, const void *data, size_t *length)
{
	size_t len = *length;
	uint8_t *ptr = (uint8_t *)data;

    do {
        ssize_t ret;
        do {
            hexdump(ptr, len);
            ret = write((int)conn, ptr, len);
            if (ret < 0)
                perror("send");
        } while ((ret < 0) && (errno == EAGAIN || errno == EINTR));
        if (ret > 0) {
            len -= ret;
            ptr += ret;
        }
        else
            return -36;
    } while (len > 0);

    *length = *length - len;
    return errSecSuccess;
}

static OSStatus SocketRead(SSLConnectionRef conn, void *data, size_t *length)
{
	size_t len = *length;
	uint8_t *ptr = (uint8_t *)data;

    do {
        ssize_t ret;
        do {
            ret = read((int)conn, ptr, len);
            if (ret < 0)
                perror("send");
        } while ((ret < 0) && (errno == EAGAIN || errno == EINTR));
        if (ret > 0) {
            len -= ret;
            ptr += ret;
        }
        else
            return -36;
    } while (len > 0);

    *length = *length - len;
    return errSecSuccess;
}

static SSLContextRef make_ssl_ref(int sock, SSLProtocol maxprot)
{
    SSLContextRef ctx = NULL;

    require_noerr(SSLNewContext(false, &ctx), out);
    require_noerr(SSLSetIOFuncs(ctx,
                                (SSLReadFunc)SocketRead, (SSLWriteFunc)SocketWrite), out);
    require_noerr(SSLSetConnection(ctx, (SSLConnectionRef)(intptr_t)sock), out);

    require_noerr(SSLSetSessionOption(ctx,
                                      kSSLSessionOptionBreakOnServerAuth, true), out);

    require_noerr(SSLSetProtocolVersionMax(ctx, maxprot), out);
    /* Tell SecureTransport to not check certs itself: it will break out of the
     handshake to let us take care of it instead. */
    require_noerr(SSLSetEnableCertVerify(ctx, false), out);

    return ctx;
out:
    if (ctx)
        SSLDisposeContext(ctx);
    return NULL;
}

static OSStatus securetransport(ssl_test_handle * ssl)
{
    OSStatus ortn;
    SSLContextRef ctx = ssl->st;
    SecTrustRef trust = NULL;
    bool got_server_auth = false, got_client_cert_req = false;

    //uint64_t start = mach_absolute_time();
    do {
        ortn = SSLHandshake(ctx);

        if (ortn == errSSLServerAuthCompleted)
        {
            require_string(!got_server_auth, out, "second server auth");
            require_string(!got_client_cert_req, out, "got client cert req before server auth");
            got_server_auth = true;
            require_string(!trust, out, "Got errSSLServerAuthCompleted twice?");
            /* verify peer cert chain */
            require_noerr(SSLCopyPeerTrust(ctx, &trust), out);
            SecTrustResultType trust_result = 0;
            /* this won't verify without setting up a trusted anchor */
            require_noerr(SecTrustEvaluate(trust, &trust_result), out);

        }    } while (ortn == errSSLWouldBlock
             || ortn == errSSLServerAuthCompleted);
    require_noerr_action_quiet(ortn, out,
                               fprintf(stderr, "Fell out of SSLHandshake with error: %d\n", (int)ortn));

    require_string(got_server_auth, out, "never got server auth");

    //uint64_t elapsed = mach_absolute_time() - start;
    //fprintf(stderr, "setr elapsed: %lld\n", elapsed);

    /*
     SSLProtocol proto = kSSLProtocolUnknown;
     require_noerr_quiet(SSLGetNegotiatedProtocolVersion(ctx, &proto), out); */

    SSLCipherSuite cipherSuite;
    require_noerr_quiet(ortn = SSLGetNegotiatedCipher(ctx, &cipherSuite), out);
    //fprintf(stderr, "st negotiated %02x\n", cipherSuite);


out:
    SSLClose(ctx);
    SSLDisposeContext(ctx);
    if (trust) CFRelease(trust);

    return ortn;
}



static ssl_test_handle *
ssl_test_handle_create(int comm, SSLProtocol maxprot)
{
    ssl_test_handle *handle = calloc(1, sizeof(ssl_test_handle));
    if (handle) {
        handle->comm = comm;
        handle->st = make_ssl_ref(comm, maxprot);
    }
    return handle;
}


struct s_server {
    char *host;
    int port;
    SSLProtocol maxprot;
} servers[] = {
    /* Good tls 1.2 servers */
    {"encrypted.google.com", 443, kTLSProtocol12 },
    {"www.amazon.com",443, kTLSProtocol12 },
    {"www.mikestoolbox.org",443, kTLSProtocol12 },
    /* servers with issues */
    {"vpp.visa.co.uk", 443, kTLSProtocol12 }, // Doesnt like SSL 3.0 in initial record layer version
    {"imap.softbank.jp",993, kTLSProtocol1 },   // softbank imap server, there are multiple servers behind this, one of them is not able to handle downgrading to TLS 1.2 properly (126.240.66.17).
    {"mobile.charter.net",993, kTLSProtocol1 }, // Support 1.2 but fail to negotiate properly
    {"mybill.vodafone.com.au", 443, kTLSProtocol1 }, /* 2056 bit server key */
};

#define NSERVERS (int)(sizeof(servers)/sizeof(servers[0]))
#define NLOOPS 1

static void
tests(void)
{
    int p;

    for(p=0; p<NSERVERS;p++) {
    for(int loops=0; loops<NLOOPS; loops++) {

        ssl_test_handle *client;

        int s;
        OSStatus r;

        s=SocketConnect(servers[p].host, servers[p].port);
        if(s<0) {
            fail("connect failed with err=%d - %s:%d (try %d)", s, servers[p].host, servers[p].port, loops);
            break;
        }

        client = ssl_test_handle_create(s, servers[p].maxprot);

        r=securetransport(client);
        ok(!r, "handshake failed with err=%ld - %s:%d (try %d)", (long)r, servers[p].host, servers[p].port, loops);

        close(s);
    } }
}

int ssl_45_tls12(int argc, char *const *argv)
{
        plan_tests(NSERVERS*NLOOPS);

        tests();

        return 0;
}