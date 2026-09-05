pi@raspberrypi:~ $ sudo v4l2-ctl --device=/dev/video0 --get-fmt-video
  sudo v4l2-ctl --device=/dev/video0 --list-formats-ext
Format Video Capture:
        Width/Height      : 2304/1296
        Pixel Format      : 'pBAA' (10-bit Bayer BGBG/GRGR Packed)
        Field             : None
        Bytes per Line    : 2880
        Size Image        : 3732480
        Colorspace        : Raw
        Transfer Function : Default (maps to None)
        YCbCr/HSV Encoding: Default (maps to ITU-R 601)
        Quantization      : Default (maps to Full Range)
        Flags             :
ioctl: VIDIOC_ENUM_FMT
        Type: Video Capture

        [0]: 'YUYV' (YUYV 4:2:2)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [1]: 'UYVY' (UYVY 4:2:2)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [2]: 'YVYU' (YVYU 4:2:2)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [3]: 'VYUY' (VYUY 4:2:2)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [4]: 'RGBP' (16-bit RGB 5-6-5)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [5]: 'RGBR' (16-bit RGB 5-6-5 BE)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [6]: 'RGBO' (16-bit A/XRGB 1-5-5-5)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [7]: 'RGBQ' (16-bit A/XRGB 1-5-5-5 BE)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [8]: 'RGB3' (24-bit RGB 8-8-8)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [9]: 'BGR3' (24-bit BGR 8-8-8)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [10]: 'RGB4' (32-bit A/XRGB 8-8-8-8)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [11]: 'BA81' (8-bit Bayer BGBG/GRGR)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [12]: 'GBRG' (8-bit Bayer GBGB/RGRG)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [13]: 'GRBG' (8-bit Bayer GRGR/BGBG)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [14]: 'RGGB' (8-bit Bayer RGRG/GBGB)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [15]: 'pBAA' (10-bit Bayer BGBG/GRGR Packed)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [16]: 'BG10' (10-bit Bayer BGBG/GRGR)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [17]: 'pGAA' (10-bit Bayer GBGB/RGRG Packed)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [18]: 'GB10' (10-bit Bayer GBGB/RGRG)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [19]: 'pgAA' (10-bit Bayer GRGR/BGBG Packed)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [20]: 'BA10' (10-bit Bayer GRGR/BGBG)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [21]: 'pRAA' (10-bit Bayer RGRG/GBGB Packed)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [22]: 'RG10' (10-bit Bayer RGRG/GBGB)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [23]: 'pBCC' (12-bit Bayer BGBG/GRGR Packed)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [24]: 'BG12' (12-bit Bayer BGBG/GRGR)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [25]: 'pGCC' (12-bit Bayer GBGB/RGRG Packed)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [26]: 'GB12' (12-bit Bayer GBGB/RGRG)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [27]: 'pgCC' (12-bit Bayer GRGR/BGBG Packed)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [28]: 'BA12' (12-bit Bayer GRGR/BGBG)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [29]: 'pRCC' (12-bit Bayer RGRG/GBGB Packed)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [30]: 'RG12' (12-bit Bayer RGRG/GBGB)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [31]: 'pBEE' (14-bit Bayer BGBG/GRGR Packed)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [32]: 'BG14' (14-bit Bayer BGBG/GRGR)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [33]: 'pGEE' (14-bit Bayer GBGB/RGRG Packed)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [34]: 'GB14' (14-bit Bayer GBGB/RGRG)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [35]: 'pgEE' (14-bit Bayer GRGR/BGBG Packed)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [36]: 'GR14' (14-bit Bayer GRGR/BGBG)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [37]: 'pREE' (14-bit Bayer RGRG/GBGB Packed)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [38]: 'RG14' (14-bit Bayer RGRG/GBGB)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [39]: 'BYR2' (16-bit Bayer BGBG/GRGR)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [40]: 'GB16' (16-bit Bayer GBGB/RGRG)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [41]: 'GR16' (16-bit Bayer GRGR/BGBG)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [42]: 'RG16' (16-bit Bayer RGRG/GBGB)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [43]: 'GREY' (8-bit Greyscale)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [44]: 'Y10P' (10-bit Greyscale (MIPI Packed))
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [45]: 'Y10 ' (10-bit Greyscale)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [46]: 'Y12P' (12-bit Greyscale (MIPI Packed))
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [47]: 'Y12 ' (12-bit Greyscale)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [48]: 'Y14P' (14-bit Greyscale (MIPI Packed))
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [49]: 'Y14 ' (14-bit Greyscale)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
        [50]: 'Y16 ' (16-bit Greyscale)
                Size: Stepwise 16x16 - 16376x16376 with step 1/1
pi@raspberrypi:~ $ sudo v4l2-ctl --device=/dev/video0 --list-formats-ext | grep -A2 "Pixel Format"
pi@raspberrypi:~ $ sudo v4l2-ctl --device=/dev/video0 --stream-mmap=1 --stream-count=1 \
      --stream-to=lib_raw.bin
<
pi@raspberrypi:~ $ xxd lib_raw.bin | head -50      # primeros bytes Linux
  xxd nuestro.bin | head -50      # primeros bytes nuestros
  diff <(xxd lib_raw.bin) <(xxd nuestro.bin) | head
-bash: xxd: command not found
-bash: xxd: command not found
-bash: xxd: command not found
-bash: xxd: command not found
pi@raspberrypi:~ $ for r in 0100 0101 0114 0202 0204 020E 0220 0342 0900 0901 034C 034E 0B8E 3400; do
    hi=$(echo $r | cut -c1-2); lo=$(echo $r | cut -c3-4)
    printf "0x$r = "
    sudo i2ctransfer -y 10 w2@0x1A 0x$hi 0x$lo r1 2>/dev/null
  done
0x0100 = 0x0101 = 0x0114 = 0x0202 = 0x0204 = 0x020E = 0x0220 = 0x0342 = 0x0900 = 0x0901 = 0x034C = 0x034E = 0x0B8E = 0x3400 = pi@raspberrypsudo cat /sys/kernel/debug/dri/0/state 2>/dev/nullv/null
plane[41]: plane-0
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=0
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[60]: plane-1
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=0
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[72]: plane-2
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=0
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[84]: plane-3
        crtc=pixelvalve-2
        fb=668
                allocated by = [fbcon]
                refcount=2
                format=RG16 little-endian (0x36314752)
                modifier=0x0
                size=1920x1080
                layers:
                        size[0]=1920x1080
                        pitch[0]=3840
                        offset[0]=0
                        obj[0]:
                                name=0
                                refcount=3
                                start=00100000
                                size=4149248
                                imported=no
        crtc-pos=1920x1080+0+0
        src-pos=1920.000000x1080.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=0
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[96]: plane-4
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=1
        color-encoding=ITU-R BT.601 YCbCr
        color-range=YCbCr full range
        color_mgmt_changed=0
plane[107]: plane-5
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=2
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[118]: plane-6
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=3
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[129]: plane-7
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=4
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[140]: plane-8
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=5
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[151]: plane-9
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=6
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[162]: plane-10
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=7
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[173]: plane-11
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=8
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[184]: plane-12
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=9
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[195]: plane-13
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=a
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[206]: plane-14
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=b
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[217]: plane-15
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=c
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[228]: plane-16
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=d
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[239]: plane-17
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=e
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[250]: plane-18
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=f
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[261]: plane-19
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=10
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[272]: plane-20
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=1
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[283]: plane-21
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=2
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[294]: plane-22
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=3
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[305]: plane-23
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=4
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[316]: plane-24
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=5
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[327]: plane-25
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=6
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[338]: plane-26
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=7
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[349]: plane-27
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=8
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[360]: plane-28
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=9
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[371]: plane-29
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=a
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[382]: plane-30
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=b
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[393]: plane-31
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=c
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[404]: plane-32
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=d
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[415]: plane-33
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=e
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[426]: plane-34
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=f
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[437]: plane-35
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=10
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[448]: plane-36
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=11
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[459]: plane-37
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=12
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[470]: plane-38
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=13
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[481]: plane-39
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=14
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[492]: plane-40
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=15
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[503]: plane-41
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=16
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[514]: plane-42
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=17
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[525]: plane-43
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=18
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[536]: plane-44
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=19
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[547]: plane-45
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=1a
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[558]: plane-46
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=1b
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[569]: plane-47
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=1c
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[580]: plane-48
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=1d
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[591]: plane-49
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=1e
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[602]: plane-50
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=1f
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[613]: plane-51
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=20
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[624]: plane-52
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=11
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[635]: plane-53
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=11
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[646]: plane-54
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=11
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
plane[657]: plane-55
        crtc=(null)
        fb=0
        crtc-pos=0x0+0+0
        src-pos=0.000000x0.000000+0.000000+0.000000
        rotation=1
        normalized-zpos=11
        color-encoding=ITU-R BT.709 YCbCr
        color-range=YCbCr limited range
        color_mgmt_changed=0
crtc[52]: txp
        enable=0
        active=0
        self_refresh_active=0
        planes_changed=0
        mode_changed=0
        active_changed=0
        connectors_changed=0
        color_mgmt_changed=0
        plane_mask=0
        connector_mask=0
        encoder_mask=0
        mode: "": 0 0 0 0 0 0 0 0 0 0 0x0 0x0
crtc[71]: pixelvalve-0
        enable=0
        active=0
        self_refresh_active=0
        planes_changed=0
        mode_changed=0
        active_changed=0
        connectors_changed=0
        color_mgmt_changed=0
        plane_mask=0
        connector_mask=0
        encoder_mask=0
        mode: "": 0 0 0 0 0 0 0 0 0 0 0x0 0x0
crtc[83]: pixelvalve-1
        enable=0
        active=0
        self_refresh_active=0
        planes_changed=0
        mode_changed=0
        active_changed=0
        connectors_changed=0
        color_mgmt_changed=0
        plane_mask=0
        connector_mask=0
        encoder_mask=0
        mode: "": 0 0 0 0 0 0 0 0 0 0 0x0 0x0
crtc[95]: pixelvalve-2
        enable=1
        active=1
        self_refresh_active=0
        planes_changed=1
        mode_changed=0
        active_changed=0
        connectors_changed=0
        color_mgmt_changed=0
        plane_mask=8
        connector_mask=1
        encoder_mask=1
        mode: "1920x1080": 60 148500 1920 2008 2052 2200 1080 1084 1089 1125 0x48 0x5
connector[33]: HDMI-A-1
        crtc=pixelvalve-2
        self_refresh_aware=0
        max_requested_bpc=8
        colorspace=Default
        broadcast_rgb=Automatic
        is_limited_range=y
        output_bpc=8
        output_format=RGB
        tmds_char_rate=148500000
connector[58]: Writeback-1
        crtc=(null)
        self_refresh_aware=0
        max_requested_bpc=0
        colorspace=Default
HVS State
        Core Clock Rate: 137600000
        Channel 0
                in use=0
                load=0
        Channel 1
                in use=1
                load=137600000
        Channel 2
                in use=0
                load=0
pi@raspberrypi:~ $  sudo cat /sys/kernel/debug/pinctrl/*/pinmux-pins | grep -E "pin (28|29|44|45)"
pin 28 (gpio28): (MUX UNCLAIMED) pinctrl-bcm2835:540
pin 29 (gpio29): (MUX UNCLAIMED) pinctrl-bcm2835:541
pin 44 (gpio44): soc:i2c0mux (GPIO UNCLAIMED) function alt1 group gpio44
pin 45 (gpio45): soc:i2c0mux (GPIO UNCLAIMED) function alt1 group gpio45

pi@raspberrypi:~ $ # (1) Stride y formato exactos
  sudo v4l2-ctl --device=/dev/video0 --get-fmt-video

  # (2) Captura raw 1 frame al mismo escenario
  sudo v4l2-ctl --device=/dev/video0 --stream-mmap=1 --stream-count=1 \
      --stream-to=lib_raw.bin

  # (3) Tamaño exacto del archivo capturado
  ls -l lib_raw.bin
Format Video Capture:
        Width/Height      : 2304/1296
        Pixel Format      : 'pBAA' (10-bit Bayer BGBG/GRGR Packed)
        Field             : None
        Bytes per Line    : 2880
        Size Image        : 3732480
        Colorspace        : Raw
        Transfer Function : Default (maps to None)
        YCbCr/HSV Encoding: Default (maps to ITU-R 601)
        Quantization      : Default (maps to Full Range)
        Flags             :
<
-rw-r--r-- 1 root root 3732480 Apr 25 18:54 lib_raw.bin

pi@raspberrypi:~ $ rpicam-still -o imagen.jpg
[0:15:32.399164752] [1362]  INFO Camera camera_manager.cpp:340 libcamera v0.7.0+rpt20260205
[0:15:32.643766104] [1365]  INFO IPAProxy ipa_proxy.cpp:180 Using tuning file /usr/share/libcamera/ipa/rpi/vc4/imx708.json
[0:15:32.660827575] [1365]  INFO Camera camera_manager.cpp:223 Adding camera '/base/soc/i2c0mux/i2c@1/imx708@1a' for pipeline handler rpi/vc4
[0:15:32.661043824] [1365]  INFO RPI vc4.cpp:445 Registered camera /base/soc/i2c0mux/i2c@1/imx708@1a to Unicam device /dev/media0 and ISP device /dev/media1
[0:15:32.661159240] [1365]  INFO RPI pipeline_base.cpp:1117 Using configuration file '/usr/share/libcamera/pipeline/rpi/vc4/rpi_apps.yaml'
Made X/EGL preview window
Made DRM preview window
Mode selection for 2048:1152:12:P
    SRGGB10_CSI2P,1536x864/0 - Score: 2600
    SRGGB10_CSI2P,2304x1296/0 - Score: 1100
    SRGGB10_CSI2P,4608x2592/0 - Score: 2000
[0:15:32.882662486] [1362]  INFO Camera camera.cpp:1215 configuring streams: (0) 2048x1152-YUV420/sYCC (1) 2304x1296-SBGGR10_CSI2P/RAW
[0:15:32.883407846] [1365]  INFO RPI vc4.cpp:620 Sensor: /base/soc/i2c0mux/i2c@1/imx708@1a - Selected sensor format: 2304x1296-SBGGR10_1X10/RAW - Selected unicam format: 2304x1296-pBAA/RAW
Mode selection for 4608:2592:12:P
    SRGGB10_CSI2P,1536x864/0 - Score: 10600
    SRGGB10_CSI2P,2304x1296/0 - Score: 8200
    SRGGB10_CSI2P,4608x2592/0 - Score: 1000
[0:15:37.980067010] [1362]  INFO Camera camera.cpp:1215 configuring streams: (0) 4608x2592-YUV420/sYCC (1) 4608x2592-SBGGR10_CSI2P/RAW
[0:15:37.981968354] [1365]  INFO RPI vc4.cpp:620 Sensor: /base/soc/i2c0mux/i2c@1/imx708@1a - Selected sensor format: 4608x2592-SBGGR10_1X10/RAW - Selected unicam format: 4608x2592-pBAA/RAW
Still capture image received