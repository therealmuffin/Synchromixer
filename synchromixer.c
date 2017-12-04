/****************************************************************************
 * Copyright (C) 2016 Muffinman
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>
#include <syslog.h>
#include <alsa/asoundlib.h>
#include "volume_mapping.h"


#ifndef NULL
    #define NULL (void *)0
#endif // #ifndef NULL

#define PROGRAM_NAME "synchromixer"
#define PROGRAM_VERSION "0.1"
#define PROGRAM_LEGAL \
"Copyright (C) 2016 Muffinman\n" \
"This is free software; see the source for copying conditions. There is NO\n" \
"warranty; not even MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n"

#define TEXT_USAGE \
"[options...]\n" \
"   -V          print version information\n" \
"   -h          print this help message\n" \
"   -d          daemonize application\n" \
"   -v          increase verbose\n" \
"   -s          set source mixer device [hw:0*]\n" \
"   -t          set source mixer control [Master*]\n" \
"   -x          set target mixer device [hw:0*]\n" \
"   -y          set target mixer control\n" \
"   -m          set maximum volume [0-100|100*]\n" \
"   -l          use linear volume control\n" \


#define LOCKFILE "/var/run/synchromixer/"PROGRAM_NAME".pid"
#define LOCKMODE (S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH)

char *defaultMixer= "hw:0";
char *defaultControl= "Master";
const char *source_mixer = NULL;
const char *source_control = NULL;
const char *target_mixer = NULL;
const char *target_control = NULL;
int normalized = 1;
int lock_file = -1;

snd_mixer_t *sndSourceHandle = NULL;
snd_mixer_elem_t *sndSourceElem = NULL;
snd_mixer_t *sndTargetHandle = NULL;
snd_mixer_elem_t *sndTargetElem = NULL;

typedef struct {
    long min;
    long max;
    long range;
} volumeSettings_t;
long volumeDiff = -1;
float volumeMultiplier = -1;

static int daemonize(void) {
    int count;
    pid_t application_pid;
    
    /* Get maximum number of file descriptors, close them, create fd to /dev/null,
     * and duplicate it twice */
    struct rlimit resource_limit;
    int fd0, fd1, fd2;
    if(getrlimit(RLIMIT_NOFILE, &resource_limit) < 0)
        fprintf(stderr, "Unable to retrieve resource limit: %s\n", strerror(errno));
    if(resource_limit.rlim_max == RLIM_INFINITY)
        resource_limit.rlim_max = 1024;
    for(count = 0; count < resource_limit.rlim_max; count++)
        close(count);
    fd0 = open("/dev/null", O_RDWR);
    fd1 = fd2 = dup(0);
    
    /* Setup syslog and give notice of execution */
    openlog(PROGRAM_NAME, LOG_NDELAY | LOG_PID, LOG_DAEMON);
    syslog(LOG_NOTICE, "Program started by User %d", getuid());
    
    /* Detach from tty and make it session leader */
    if((application_pid = fork()) < 0) {
        syslog(LOG_ERR, "Failed to detach from TTY: %s", strerror(errno));
        return EXIT_FAILURE;
    }
    else if(application_pid > 0) {
        closelog();
        exit(EXIT_SUCCESS);   
    }
    
    /* Create new session and make current process its leader */
    setsid();
    
    struct sigaction signal_action;
    signal_action.sa_handler = SIG_IGN;
    sigemptyset(&signal_action.sa_mask);
    signal_action.sa_flags = 0;
    if(sigaction(SIGHUP, &signal_action, NULL) < 0) {
        syslog(LOG_ERR, "Failed to ignore SIGHUP: %s", strerror(errno));
        closelog();
        return EXIT_FAILURE;
    }

    /* Second forking: attach process to init */
    if((application_pid = fork()) < 0) {
        syslog(LOG_ERR, "Failed to attach to init: %s", strerror(errno));
        closelog();
        return EXIT_FAILURE;
    }
    else if(application_pid > 0) {
        closelog();
        exit(EXIT_SUCCESS);
    }
    
    return EXIT_SUCCESS;
} /* end daemonize */

int initMixer(snd_mixer_t **sndHandle, snd_mixer_elem_t **sndElem, const char *mixerDevice, 
const char *mixerControl, volumeSettings_t *volumeSettings) {
    snd_mixer_selem_id_t* snd_sid;
    snd_mixer_selem_id_alloca(&snd_sid);
    
    int mixIndex = 0;
    int status = -2;

    /* sets simple-mixer index and name */
    snd_mixer_selem_id_set_index(snd_sid, mixIndex);
    snd_mixer_selem_id_set_name(snd_sid, mixerControl);
    
    if((status = snd_mixer_open(sndHandle, 0)) < 0) {
        syslog(LOG_ERR, "Failed to open mixer: %s (%i)", snd_strerror(status), status);
        return EXIT_FAILURE;
    }
    if((status = snd_mixer_attach(*sndHandle, mixerDevice)) < 0) {
        syslog(LOG_ERR, "Failed to attach device to mixer %s: %s (%i)", 
            mixerDevice, snd_strerror(status), status);
        return EXIT_FAILURE;
    }
    if((status = snd_mixer_selem_register(*sndHandle, NULL, NULL)) < 0) {
        syslog(LOG_ERR, "Failed to register mixer element class: %s (%i)", 
            snd_strerror(status), status);
        return EXIT_FAILURE;
    }
    if((status = snd_mixer_load(*sndHandle)) < 0) {
        syslog(LOG_ERR, "Failed to load mixer elements: %s (%i)", 
            snd_strerror(status), status);
        return EXIT_FAILURE;
    }
    if((*sndElem = snd_mixer_find_selem(*sndHandle, snd_sid)) == NULL) {
        syslog(LOG_ERR, "Failed to find mixer element %s", mixerControl);
        return EXIT_FAILURE;
    }
    snd_mixer_selem_get_playback_volume_range(*sndElem, &(*volumeSettings).min, &(*volumeSettings).max);
    volumeSettings->range = volumeSettings->max - volumeSettings->min;
    syslog(LOG_DEBUG, "Alsa volume range, min, max: %i, %i, %i", volumeSettings->range, 
        volumeSettings->min, volumeSettings->max);
    
    return EXIT_SUCCESS;
}

int processVolume(void) {
    static long volume_old;
    long volume = -1;
    double targetVolume;
    int status = -2;
    float temp;

    if((snd_mixer_selem_get_playback_volume(sndSourceElem, 0, &volume)) < 0) {
            syslog(LOG_ERR, "Failed to get source mixer: %s (%i)", 
                snd_strerror(status), status);
            return EXIT_FAILURE;
    }
    
    if(volume == volume_old)
        return EXIT_SUCCESS;
    volume_old = volume;
    
    if(normalized) {
        targetVolume = 0.01 * ((volume * volumeMultiplier) - volumeDiff);
        if((set_normalized_playback_volume(sndTargetElem, targetVolume, 0)) != 0) {
            syslog(LOG_WARNING, "Setting target mixer failed");
            return EXIT_FAILURE;
        }
    }
    else {
        targetVolume = (volume * volumeMultiplier) - volumeDiff;
        if((snd_mixer_selem_set_playback_volume_all(sndTargetElem, (long) targetVolume)) != 0) {
            syslog(LOG_WARNING, "Setting target mixer failed");
            return EXIT_FAILURE;
        }
    }
    syslog(LOG_WARNING, "Setting target volume to level (normalized): %i (%.2f)", volume, targetVolume);

    return EXIT_SUCCESS;
}

int watchSourceMixer(void) {
    int status = -2;
    int events = -1;
    
    if(processVolume() == EXIT_FAILURE) {
        raise(SIGTERM);
        pause();
    }
    
    while(1) {
        if((status = snd_mixer_wait(sndSourceHandle, -1)) < 0) {
            syslog(LOG_ERR, "Failed waiting for mixer event: %s (%i)", 
                snd_strerror(status), status);
            raise(SIGTERM);
            pause();
        }
        
        /* clears all pending mixer events */
        events = snd_mixer_handle_events(sndSourceHandle);
        /* We're relying here on a stereo mixer from snd_dummy. This causes
         * mixer events to be doubled. Therefore check if the sound
         * level has changed and if not continue. */
        
        if(processVolume() == EXIT_FAILURE) {
            raise(SIGTERM);
            pause();
        }
    }
    /* Shouldn't have gotten here */
    raise(SIGTERM);
    pause();
}

void deinitMixer(snd_mixer_t *sndHandle) {
    /* Freeing resources */
    if(sndHandle != NULL)
        snd_mixer_close(sndHandle);
}

static void terminate(int signum) {
    deinitMixer(sndSourceHandle);
    deinitMixer(sndTargetHandle);

    if(lock_file > 0) {
        lockf(lock_file, F_ULOCK, 0);
        close(lock_file);
        unlink(LOCKFILE);
    }
    
    syslog(LOG_NOTICE, "Program terminated: %s (%i)", strsignal(signum), signum);
    closelog();
    exit(EXIT_SUCCESS);
} /* end terminate */

int main(int argc, char *argv[]) {
    int status = 0;
    int daemon = 0;
    long max_volume = 100;
    
    setlogmask(LOG_UPTO(LOG_NOTICE));
    
    int option = 0;
    opterr = 0;
    while((option = getopt(argc, argv, "Vlhdv:s:t:x:y:m:")) != -1)
        switch(option) {
            case 'v':
                if(strcmp(optarg, "0") == 0)
                    setlogmask(LOG_UPTO(LOG_ERR));
                else if(strcmp(optarg, "1") == 0)
                    setlogmask(LOG_UPTO(LOG_INFO));
                else if(strcmp(optarg, "2") == 0)
                    setlogmask(LOG_UPTO(LOG_DEBUG));
                break;
            case 'V':
                fprintf(stdout, "%s %s\n\n%s\n", PROGRAM_NAME, PROGRAM_VERSION, PROGRAM_LEGAL);
                exit(EXIT_SUCCESS);
            case 'h':
                fprintf(stdout, "Usage: %s %s", argv[0], TEXT_USAGE);
                exit(EXIT_SUCCESS);   
            case 'd':
                daemon = 1;
                break;
            case 'l':
                normalized = 0;
                break;
            case 's':
                source_mixer = optarg;
                break;
            case 't':
                source_control = optarg;
                break;
            case 'x':
                target_mixer = optarg;
                break;
            case 'y':
                target_control = optarg;
                break;
            case 'm':
                errno = 0;
                status = strtol(optarg, NULL, 10);
                if(errno == 0) {
                    max_volume = status;
                }
                else {
                    fprintf(stderr, "Invalid max volume '%s'", optarg);
                    exit(EXIT_SUCCESS);
                }
                if(max_volume > 100 || max_volume < 0) {
                    fprintf(stderr, "Volume out of range '%i'", max_volume);
                    exit(EXIT_SUCCESS);
                }
                break;
            case '?':
                if(optopt == 'v' || optopt == 's' || optopt == 't' || optopt == 'x' || optopt == 'y' || optopt == 'm')
                    fprintf(stderr, "You forgot to set the option for '-%c'.\n", optopt);
                else if(isprint(optopt))
                    fprintf(stderr, "Unknown option `-%c'.\n", optopt);
                else
                    fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
                exit(EXIT_FAILURE);
            default:
                exit(EXIT_FAILURE);
        }
    if(source_mixer == NULL)
        source_mixer = defaultMixer;
    if(target_mixer == NULL)
        target_mixer = defaultMixer;
    if(source_control == NULL)
        source_control = defaultControl;
    if(target_control == NULL) {
        fprintf(stderr, "Target mixer control must be set!!");
        exit(EXIT_FAILURE);
    }
    if(strcmp(source_mixer, target_mixer) == 0 && strcmp(source_control, target_control) == 0) {
        fprintf(stderr, "Source and target control can not be equal if source and target mixer are! '-%s'", target_control);
        exit(EXIT_FAILURE);
    }
    /* All files created without revoked permissions (thus result: 0666) */
    umask(0);
    
    if(daemon) {
        if((status = daemonize()) == EXIT_FAILURE)
            exit(EXIT_FAILURE);
    }
    else {
        /* Setup syslog, print also to stderr, and give notice of execution */
        openlog(PROGRAM_NAME, LOG_NDELAY | LOG_PID | LOG_PERROR, LOG_DAEMON);
        syslog(LOG_NOTICE, "Program started by User %d", getuid());
    }
        
    /* Lock process and print pid to lock file */
    if((lock_file = open(LOCKFILE, O_RDWR|O_CREAT|O_CLOEXEC, LOCKMODE)) < 0) {
        syslog(LOG_ERR, "Failed to open lock file: %s (%s)", LOCKFILE, strerror(errno));
        closelog();
        exit(EXIT_FAILURE);
    }
    if(lockf(lock_file, F_TLOCK, 0) < 0) {
        syslog(LOG_WARNING, "Exiting: only one instance of this application can run: %s", strerror(errno));
        closelog();
        exit(EXIT_SUCCESS);
    }
    ftruncate(lock_file, 0);
    dprintf(lock_file, "%d\n", getpid());
    
    struct sigaction signal_action;
    signal_action.sa_handler = terminate;
    sigfillset(&signal_action.sa_mask);
    signal_action.sa_flags = 0;
    if(sigaction(SIGINT, &signal_action, NULL) < 0 || sigaction(SIGTERM, &signal_action, NULL) < 0) {
        syslog(LOG_ERR, "Failed to ignore signals: %s", strerror(errno));
        closelog();
        exit(EXIT_FAILURE);
    }
    
    volumeSettings_t source;
    if(initMixer(&sndSourceHandle, &sndSourceElem, source_mixer, source_control, &source) == EXIT_FAILURE) {
        raise(SIGTERM);
        pause();
    }
    volumeSettings_t target;
    if(initMixer(&sndTargetHandle, &sndTargetElem, target_mixer, target_control, &target) == EXIT_FAILURE) {
        raise(SIGTERM);
        pause();
    }
    
    // correct max volume for... what exactly?? seems to work
    if(max_volume <= 1) {
        max_volume = 0;
        return;
    }
    max_volume = 50*log10(max_volume);
    
    
    volumeDiff = source.min - target.min;
    if(normalized) {
        volumeMultiplier = ((float)100 / source.range)*((float)max_volume/100);
    }
    else {
        volumeMultiplier = ((float)target.range / source.range)*((float)max_volume/100);
    }
    syslog(LOG_DEBUG, "Multiplier and differential: %f, %i", volumeMultiplier, volumeDiff);
    
    watchSourceMixer();

    raise(SIGTERM);
    pause();
    exit(EXIT_SUCCESS);
}

