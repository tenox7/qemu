# AIX 7.2 on pseries

`run.sh` boots AIX 7.2 on an emulated POWER8 pSeries LPAR from `hdisk0.qcow2` (created on first run), falling back to the install DVD next to it; `./run.sh cd` forces the DVD. Use the TL5 SP3 DVD `AIX_v7.2_Install_7200-05-03-2147_DVD_1_of_2.iso`, TL5 SP10 hangs right after the kernel banner.
