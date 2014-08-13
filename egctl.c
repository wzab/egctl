/*
 * egctl - EnerGenie EM-PMS-LAN control utility
 *
 * Copyright (c) 2014 Vitaly Sinilin <vs@kp4.ru>
 *
 * Published under the terms of the MIT License,
 * see the included COPYING file.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define TASK_LEN            4
#define RES_LEN             4
#define STATCRYP_LEN        4
#define CTRLCRYP_LEN        4
#define KEY_LEN             8

#define STATE_ON            0x11
#define STATE_ON_NO_VOLTAGE 0x12
#define STATE_OFF           0x22
#define STATE_OFF_VOLTAGE   0x21

#define SWITCH_ON           0x01
#define SWITCH_OFF          0x02
#define DONT_SWITCH         0x04

#define SOCKET_COUNT 4      /* AC power sockets, not network ones ;) */

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

typedef struct
{
    struct sockaddr_in addr;
    uint8_t            key[KEY_LEN];
} Config;

typedef struct
{
    uint8_t socket[SOCKET_COUNT];
} Status, Controls;

typedef struct
{
    uint8_t task[TASK_LEN];
    uint8_t key[KEY_LEN];
} Session;

const char *g_egtabs[] =
{
    NULL,           /* placeholder for ~/.egtab */
    "/etc/egtab"
};

void vfatal(const char *fmt, va_list ap)
{
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfatal(fmt, ap);
    va_end(ap);
}

void warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

#ifdef DEBUG
void dbg4(const char *name, const uint8_t *buf)
{
    printf("%8s: 0x%02X 0x%02X 0x%02X 0x%02X\n",
           name, buf[0], buf[1], buf[2], buf[3]);
}
#else
#define dbg4(n,b)
#endif

inline uint16_t swap16(uint16_t val)
{
    return ((val & 0xFF) << 8) | ((val & 0xFF00) >> 8);
}

inline uint16_t host_to_le16(uint16_t hostu16)
{
    return swap16(htons(hostu16));
}

void xread(int fd, void *buf, size_t count)
{
    ssize_t ret = read(fd, buf, count);

    if (ret == (ssize_t)count) {
        return;
    } else if (ret == -1) {
        if (errno != EINTR)
            fatal("Unable to read from socket: %s", strerror(errno));
        else
            ret = 0;
    }

    xread(fd, (char *)buf + ret, count - ret);
}

void xwrite(int fd, const void *buf, size_t count)
{
    ssize_t ret = write(fd, buf, count);

    if (ret == (ssize_t)count) {
        return;
    } else if (ret == -1) {
        if (errno != EINTR)
            fatal("Unable to write to socket: %s", strerror(errno));
        else
            ret = 0;
    }

    xwrite(fd, (char *)buf + ret, count - ret);
}

char *get_token(char **str)
{
    char *tok = *str;

    if (tok) {
        /* strip leading delimiters */
        tok += strspn(tok, " \t");

        if (*tok == '\0') { /* no tokens */
            *str = NULL;
            tok = NULL;
        } else {
            char *eot = tok + strcspn(tok, " \t");
            if (*eot == '\0') { /* last token */
                *str = NULL;
            } else {
                *eot = '\0';
                *str = eot + 1;
            }
        }
    }

    return tok;
}

char *xget_token(char **str, const char *fmt, ...)
{
    char *tok = get_token(str);

    if (!tok) {
        va_list ap;
        va_start(ap, fmt);
        vfatal(fmt, ap);
        va_end(ap);
    }

    return tok;
}

char *get_personal_egtab_name(void)
{
    static char egtab[1024] = "/dev/null";
    struct passwd *pwd = getpwuid(getuid());

    if (pwd) {
        snprintf(egtab, sizeof(egtab), "%s/.egtab", pwd->pw_dir);
    } else {
        warn("Unable to determine user home directory");
    }

    return egtab;
}

int get_device_entry(const char *name, FILE *fp, Config *conf)
{
    char buf[1024];
    char *line;

    while ((line = fgets(buf, sizeof(buf), fp)) != NULL) {
        char *tabname;

        if (line[0] == '#')
            continue;

        line[strcspn(line, "\n")] = '\0';

        tabname = get_token(&line);

        if (tabname && !strcmp(tabname, name)) {
            size_t keylen;
            char *tok;

            tok = xget_token(&line, "%s: IP address isn't specified", name);
            conf->addr.sin_addr.s_addr = inet_addr(tok);
            if (conf->addr.sin_addr.s_addr == INADDR_NONE) {
                /* It is ok that INADDR_NONE screens 255.255.255.255, since
                 * this address isn't appropriate here anyway. */
                fatal("%s: invalid IP address specified", name);
            }

            tok = xget_token(&line, "%s: TCP port isn't specified", name);
            conf->addr.sin_port = htons(atoi(tok));

            tok = xget_token(&line, "%s: password isn't specified", name);
            keylen = strlen(tok);
            if (keylen > sizeof(conf->key)) {
                warn("%s: password too long, only first %u chars "
                     "will be considered", name, sizeof(conf->key));
                keylen = sizeof(conf->key);
            }
            /* Key should be padded with trailing spaces */
            memset(conf->key, 0x20, sizeof(conf->key));
            memcpy(conf->key, tok, keylen);

            conf->addr.sin_family = AF_INET;
            return 1;
        }
    }

    return 0;
}

Config get_device_conf(const char *name)
{
    Config conf;
    int opened_tabs = 0;
    int ent_found = 0;
    size_t i;

    for (i = 0; !ent_found && i < ARRAY_SIZE(g_egtabs); i++) {
        FILE *fp = fopen(g_egtabs[i], "r");

        if (fp != NULL) {
            opened_tabs++;
            ent_found = get_device_entry(name, fp, &conf);
            fclose(fp);
        }
    }

    if (opened_tabs == 0)
        fatal("Unable to open any config file");

    if (!ent_found)
        fatal("%s: unknown device", name);

    return conf;
}

int create_socket(const struct sockaddr_in *addr)
{
    int ret;
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (sock == -1)
        fatal("Unable to create socket: %s", strerror(errno));

    ret = connect(sock, (const struct sockaddr *)addr, sizeof(*addr));

    if (ret != 0)
        fatal("Unable to connect: %s", strerror(errno));

    return sock;
}

void establish_connection(int sock)
{
    int i, ret;
    fd_set fds;

    /* When the device is still on timeout from a previous session
     * it doesn't respond to the first Start condition packet. So
     * we will take several attempts. */

    for (i = 0; i < 4; i++) {
        struct timeval tv = { 0, 125000 };

        xwrite(sock, "\x11", 1);
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        ret = select(sock + 1, &fds, NULL, NULL, &tv);

        if (ret == 1)
            return;
    }

    fatal("Unable to establish connection with device");
}

Session authorize(int sock, const uint8_t key[])
{
    Session s;
    uint8_t res[RES_LEN];
    uint16_t word;
    fd_set fds;
    struct timeval tv = { 4, 0 };
    int ret;

    xread(sock, &s.task, sizeof(s.task));
    dbg4("task", s.task);

    word = ((s.task[0]^key[2])*key[0])^(key[6]|(key[4]<<8))^s.task[2];
    word = host_to_le16(word);
    memcpy(res, &word, 2);
    word = ((s.task[1]^key[3])*key[1])^(key[7]|(key[5]<<8))^s.task[3];
    word = host_to_le16(word);
    memcpy(&res[2], &word, 2);
    dbg4("res", res);

    xwrite(sock, &res, sizeof(res));

    /* The protocol doesn't specify any explicit response on failed
     * authorization. So timeout is the only way to find out that
     * authorization hasn't been successful. */

    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    ret = select(sock + 1, &fds, NULL, NULL, &tv);

    if (ret != 1)
        fatal("Authorization failed");

    memcpy(&s.key, key, sizeof(s.key));

    return s;
}

Status decrypt_status(const uint8_t statcryp[], Session s)
{
    Status st;
    size_t i;

    for (i = 0; i < SOCKET_COUNT; i++)
        st.socket[i] =
            (((statcryp[3-i] - s.key[1])^s.key[0]) - s.task[3])^s.task[2];

    return st;
}

Status recv_status(int sock, Session s)
{
    uint8_t statcryp[STATCRYP_LEN];
    xread(sock, &statcryp, sizeof(statcryp));
    dbg4("statcryp", statcryp);
    return decrypt_status(statcryp, s);
}

uint8_t map_action(unsigned int sock_num, uint8_t cur_state, const char *action)
{
    if (!strcmp(action, "on")) {
        return SWITCH_ON;
    } else if (!strcmp(action, "off")) {
        return SWITCH_OFF;
    } else if (!strcmp(action, "toggle")) {
        switch (cur_state) {
            case STATE_OFF:
            case STATE_OFF_VOLTAGE:
                return SWITCH_ON;
            case STATE_ON:
            case STATE_ON_NO_VOLTAGE:
                return SWITCH_OFF;
            default:
                fprintf(stderr, "Cannot toggle socket %u", sock_num);
                return DONT_SWITCH;
        }
    } else if (!strcmp(action, "left")) {
        return DONT_SWITCH;
    }

    fatal("Invalid action for socket %u: %s", sock_num, action);

    /* Should never be here, but compiler is uncertain about this. */
    return DONT_SWITCH;
}

void send_controls(int sock, Session s, Controls ctrl)
{
    size_t i;
    uint8_t ctrlcryp[CTRLCRYP_LEN];

    /* Encrypt controls */
    for (i = 0; i < SOCKET_COUNT; i++)
        ctrlcryp[i] =
            (((ctrl.socket[3-i]^s.task[2]) + s.task[3])^s.key[0]) + s.key[1];

    xwrite(sock, &ctrlcryp, sizeof(ctrlcryp));
}

void close_session(int sock)
{
    /* Empirically found way to close session w/o 4 second timeout on
     * the device side is to send some invalid sequence. This helps
     * to avoid a hiccup on subsequent run of the utility. */
    xwrite(sock, "\x11", 1);
}

void dump_status(Status st)
{
    size_t i;
    const char *state;

    for (i = 0; i < SOCKET_COUNT; i++) {
        switch (st.socket[i]) {
            case STATE_ON:
                state = "on";
                break;
            case STATE_ON_NO_VOLTAGE:
                state = "on (no voltage!)";
                break;
            case STATE_OFF:
                state = "off";
                break;
            case STATE_OFF_VOLTAGE:
                state = "off (VOLTAGE IS PRESENT!)";
                break;
            default:
                state = "unknown";
                break;
        }
        printf("socket %u: %s\n", i+1, state);
    }
}

int main(int argc, char *argv[])
{
    int sock;
    Config conf;
    Session sess;
    Status status;

    if (argc != 2 && argc != 6) {
        fatal("egctl 0.1: EnerGenie EG-PMS-LAN control utility\n\n"
              "Usage: egctl NAME [S1 S2 S3 S4]\n"
              "  NAME is the name of the device in the egtab file\n"
              "  Sn is an action to perform on n-th socket: "
              "on, off, toggle or left");
    }

    g_egtabs[0] = get_personal_egtab_name();

    conf = get_device_conf(argv[1]);
    sock = create_socket(&conf.addr);
    establish_connection(sock);
    sess = authorize(sock, conf.key);
    status = recv_status(sock, sess);

    if (argc == 2) {
        dump_status(status);
    } else if (argc == 6) {
        Controls ctrl;
        size_t i;

        for (i = 0; i < SOCKET_COUNT; i++)
            ctrl.socket[i] = map_action(i+1, status.socket[i], argv[i+2]);

        send_controls(sock, sess, ctrl);

        /* Dump updated status */
        dump_status(recv_status(sock, sess));
    }

    close_session(sock);
    close(sock);

    return EXIT_SUCCESS;
}
