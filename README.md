
#Introduction
------------
The latest source code may be obtained from Google Code:
http://code.google.com/p/softflowd/
Author's github:
https://github.com/djmdjm

What I want is only "pcap" related code, so remove netflow part.

Will store data in local RRD and generate statistics graph on local machine.


#Installing
----------
Building softflowd should be as simple as typing:

yum install libpcap-devel

./configure
make
make install

./softflowd -i eno16777984 -D

