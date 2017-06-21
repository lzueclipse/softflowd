Welcome to softflowd, a flow-based network monitor.

#Introduction
------------
The latest source code may be obtained from Google Code:
http://code.google.com/p/softflowd/
Author's github:
https://github.com/djmdjm

#Installing
----------
Building softflowd should be as simple as typing:
yum install libpcap-devel

./configure
make
make install

./softflowd -i eno16777984 -n localhost:2055 -v 9 -D
