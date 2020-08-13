
// This file keeps C++ format, but it should be only included inside a C++ file.
// Prior to #include, the following "LOGGER" macro should be defined:
// #define LOGGER(upname, shortname, numeric) <whatever you need>
// #defing LOGGER_H(upname, shortname, numeric) <same, but for hidden logs>


LOGGER(GENERAL,   gg, 0);
LOGGER(SOCKMGMT,  sm, 1);
LOGGER(CONN,      ca, 2);
LOGGER(XTIMER,    xt, 3);
LOGGER(TSBPD,     ts, 4);
LOGGER(RSRC,      rs, 5);
// Haicrypt logging - usually off.
LOGGER_H(HAICRYPT,hc, 6);
LOGGER(CONGEST,   cc, 7);
LOGGER(PFILTER,   pf, 8);
// APPLOG=10 - defined in apps, this is only a stub to lock the value
LOGGER_H(APPLOG,  ap, 10);
LOGGER(API_CTRL,  ac, 11);
LOGGER(QUE_CTRL,  qc, 13);
LOGGER(EPOLL_UPD, ei, 16);

LOGGER(API_RECV,  ar, 21);
LOGGER(BUF_RECV,  br, 22);
LOGGER(QUE_RECV,  qr, 23);
LOGGER(CHN_RECV,  kr, 24);
LOGGER(GRP_RECV,  gr, 25);

LOGGER(API_SEND,  as, 31);
LOGGER(BUF_SEND,  bs, 32);
LOGGER(QUE_SEND,  qs, 33);
LOGGER(CHN_SEND,  ks, 34);
LOGGER(GRP_SEND,  gs, 35);

LOGGER(INTERNAL,  ip, 41);
LOGGER(QUE_MGMT,  qm, 43);
LOGGER(CHN_MGMT,  cm, 44);
LOGGER(GRP_MGMT,  gm, 45);
LOGGER(EPOLL_API, ea, 46);

