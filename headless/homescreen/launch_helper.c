/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Home screen launch helper: starts ProsperoEden for a game's home screen tile.
 *
 * A tile (headless/homescreen/tile) is an app of its own, and the console refuses to start
 * another title from inside a running app (0x80940010 on firmware 13.60, seen on a console).
 * The same call from a payload process starts it (verified there too). So the tile writes the
 * request and this job, sends this payload to the ELF loader (TCP 9021) and closes:
 *
 *   /data/prosperoeden/homescreen/launch-job.txt
 *       start=PPSA99008     the title to start
 *       after=FAKE10001     the tile, which must have closed first
 *       created=1791350798  console seconds when the job was written (one over a minute old is
 *                           not followed); the job is deleted before anything else
 *
 * The start is the one unjail-ps5app-payload's launch worker makes (Program.cs,
 * LaunchWorkerEntry): sceSystemServiceLaunchApp(title, {NULL}, {size 0x20, foreground user}).
 * Every step goes to /data/prosperoeden/logs/homescreen-helper.log; a failure also shows a
 * notification, since nothing else is on screen to say it.
 */

#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The launch parameter: 0x20 bytes (SharpProspero Platform/AppLauncher.cs SceAppLaunchCtx;
 * Mihawk-99's ps5vkctl lnc_app_param_t). */
typedef struct launch_param
{
    uint32_t size;
    int32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint64_t check_flag;
} launch_param_t;
_Static_assert(sizeof(launch_param_t) == 0x20, "launch parameter is 0x20 bytes");

/* A notification as ps5-native-app-boilerplate's demo sends it (src/demo_renderer.cpp). */
typedef struct notification
{
    uint8_t reserved[45];
    char message[3075];
} notification_t;

/* The payload SDK's stubs; declared as ps5vkctl (payload/ps5vkctl/main.c) declares them. */
int sceSystemServiceLaunchApp(const char *title_id, const char *argv[], launch_param_t *param);
uint32_t sceLncUtilGetAppIdOfRunningBigApp(void);
int sceLncUtilGetAppTitleId(uint32_t app_id, char *title_id);
int sceUserServiceInitialize(void *param);
int sceUserServiceGetForegroundUser(int *user_id);
int sceKernelSendNotificationRequest(uint32_t device, void *request, size_t size, int blocking);

#define JOB_PATH "/data/prosperoeden/homescreen/launch-job.txt"
#define LOG_PATH "/data/prosperoeden/logs/homescreen-helper.log"
#define LOG_PREVIOUS "/data/prosperoeden/logs/homescreen-helper.prev.log"
#define LOG_LIMIT (256 * 1024)
#define MAX_AGE_SECONDS 60
#define NO_APP 0xffffffffu

static FILE *g_log;

static void say(const char *format, ...)
{
    if (g_log == NULL)
        return;
    char stamp[32];
    const time_t now = time(NULL);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", gmtime(&now));
    va_list arguments;
    va_start(arguments, format);
    fprintf(g_log, "%s ", stamp);
    vfprintf(g_log, format, arguments);
    fputc('\n', g_log);
    va_end(arguments);
    fflush(g_log);
}

static void notify(const char *message)
{
    static notification_t request;
    memset(&request, 0, sizeof(request));
    snprintf(request.message, sizeof(request.message), "%s", message);
    (void)sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

/* The running big app's title, or "" when none runs. */
static void running_title(char title[16])
{
    title[0] = '\0';
    const uint32_t app_id = sceLncUtilGetAppIdOfRunningBigApp();
    if (app_id == NO_APP || app_id == 0)
        return;
    char buffer[32] = {0};
    if (sceLncUtilGetAppTitleId(app_id, buffer) == 0)
        snprintf(title, 16, "%.9s", buffer);
}

static int valid_title(const char *title)
{
    if (strlen(title) != 9)
        return 0;
    for (int i = 0; i < 9; i++)
    {
        const char c = title[i];
        if (i < 4 ? (c < 'A' || c > 'Z') : (c < '0' || c > '9'))
            return 0;
    }
    return 1;
}

typedef struct job
{
    char start[16];
    char after[16];
    long long created;
} job_t;

/* Reads and deletes the job. 0 when it is complete and fresh. */
static int take_job(job_t *job)
{
    memset(job, 0, sizeof(*job));
    FILE *file = fopen(JOB_PATH, "r");
    if (file == NULL)
    {
        say("no job at %s", JOB_PATH);
        return -1;
    }
    char line[128];
    while (fgets(line, sizeof(line), file) != NULL)
    {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "start=", 6) == 0)
            snprintf(job->start, sizeof(job->start), "%.9s", line + 6);
        else if (strncmp(line, "after=", 6) == 0)
            snprintf(job->after, sizeof(job->after), "%.9s", line + 6);
        else if (strncmp(line, "created=", 8) == 0)
            sscanf(line + 8, "%lld", &job->created);
    }
    fclose(file);
    if (remove(JOB_PATH) != 0)
    {
        say("the job cannot be removed; not followed");
        return -1;
    }
    const long long age = (long long)time(NULL) - job->created;
    if (age > MAX_AGE_SECONDS || age < -5)
    {
        say("the job was written %lld s ago (stale); not followed", age);
        return -1;
    }
    if (!valid_title(job->start) || (job->after[0] != '\0' && !valid_title(job->after)))
    {
        say("the job names no valid title (start=[%s] after=[%s])", job->start, job->after);
        return -1;
    }
    return 0;
}

/* Starts a title for the foreground user and waits until the console runs it. */
static int start_title(const char *title)
{
    for (int attempt = 1; attempt <= 3; attempt++)
    {
        int user = -1;
        const int user_rc = sceUserServiceGetForegroundUser(&user);
        launch_param_t param;
        memset(&param, 0, sizeof(param));
        param.size = sizeof(param);
        param.user_id = user;
        const char *argv[] = {NULL};
        const int rc = sceSystemServiceLaunchApp(title, argv, &param);
        say("start %s (attempt %d, user %d, user rc 0x%08x) -> 0x%08x", title, attempt, user,
            (unsigned)user_rc, (unsigned)rc);
        char running[16];
        for (int wait = 0; wait < 40; wait++)
        {
            running_title(running);
            if (strcmp(running, title) == 0)
            {
                say("%s is running", title);
                return 0;
            }
            usleep(250000);
        }
        say("%s is not running after 10 s (running=[%s])", title, running);
        sleep(2);
    }
    return -1;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    mkdir("/data/prosperoeden/logs", 0777);
    struct stat info;
    if (stat(LOG_PATH, &info) == 0 && info.st_size > LOG_LIMIT)
        rename(LOG_PATH, LOG_PREVIOUS);
    g_log = fopen(LOG_PATH, "a");
    say("---- launch helper, pid %d", (int)getpid());
    say("user service init 0x%08x", (unsigned)sceUserServiceInitialize(NULL));

    job_t job;
    if (take_job(&job) != 0)
        return 1;
    say("job: start=%s after=%s", job.start, job.after[0] ? job.after : "-");

    // The tile must close first: the console refuses a start while it runs.
    char running[16];
    if (job.after[0] != '\0')
    {
        int waited = 0;
        for (; waited < 150; waited++)
        {
            running_title(running);
            if (strcmp(running, job.after) != 0)
                break;
            usleep(100000);
        }
        say("%s closed after %d ms (running=[%s])", job.after, waited * 100, running);
        sleep(1);
    }
    running_title(running);
    if (strcmp(running, job.start) == 0)
    {
        say("%s is already running", job.start);
        return 0;
    }
    if (running[0] != '\0')
        say("another app runs (%s); asking anyway", running);
    if (start_title(job.start) != 0)
    {
        say("giving up: %s did not start", job.start);
        notify("ProsperoEden could not be started. If it is open in the background, close it "
               "and choose the game again.");
        return 1;
    }
    return 0;
}
