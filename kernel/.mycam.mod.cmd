savedcmd_mycam.mod := printf '%s\n'   mycam_main.o mycam_video.o mycam_vb2.o mycam_urb.o | awk '!x[$$0]++ { print("./"$$0) }' > mycam.mod
