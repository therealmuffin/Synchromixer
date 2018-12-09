# Synchromixer

Synchromixer enables setting a (proxy) mixer in between an audio application and an Alsa mixer control. Synchromixer includes functionality to set a maximum volume level on the 'target' mixer device while retaining the maximum volume range in the controlling audio application.

## Usage

```
[options...]

   -V          print version information
   -h          print this help message
   -d          daemonize application
   -v          increase verbose
   -s          set source mixer device [hw:0*]
   -t          set source mixer control [Master*]
   -x          set target mixer device [hw:0*]
   -y          set target mixer control
   -m          set maximum volume [0-100|100*]
   -l          use linear volume control
```
## Compilation

gcc synchromixer.c volume_mapping.c -lasound -o synchromixer -lm