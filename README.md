# Mini Container Runtime (OS Jackfruit Project)

## Team
- Spoorthy M(PES1UG24AM280)
- Shristi Shukla(PES1UG24AM268)

## What we built
We built a basic container runtime using Linux system calls to understand how containers work internally.

## Features
- Start a container
- List containers using ps
- Stop a container
- View logs

## Concepts used
- clone()
- PID namespace
- UTS namespace
- Mount namespace
- chroot()
- Unix sockets

## How it works
- A supervisor process runs in the background
- Commands like start, ps, stop, and logs talk to it using a socket
- Containers are created using clone() and chroot()

## How to run

### Build
`make`

### Start supervisor
`sudo ./engine supervisor ~/rootfs-base`

### Run commands
```text
./engine start alpha ~/rootfs-alpha /bin/sh
./engine ps
./engine stop alpha
./engine ps
./engine start beta ~/rootfs-alpha /bin/ls
./engine logs beta
