PREFIX?=        /usr/local
PTHREAD_LIBS?=	-lpthread
LOCALBASE?=     /usr/local
BINDIR=         ${PREFIX}/sbin
PROG=		audiocat
MAN=
CFLAGS=		-I${LOCALBASE}/include
SRCS+=		audiocat.c
LDFLAGS+=	${PTHREAD_LIBS}

.include <bsd.prog.mk>
