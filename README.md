
#Introduction
------------
Original softflowd may be obtained from Google Code:

http://code.google.com/p/softflowd/

Author's github:

https://github.com/djmdjm

#Mini-softflowd
-------------
What I want is only "pcap" related code, so remove netflow part.

Will store data in influxdb via HTTP RESTFUL API.


#Installing
----------
Building softflowd should be as simple as typing:

yum install libpcap-devel

./configure
make

./softflowd -i eno16777984 -D

#infludb
----------
wget https://dl.influxdata.com/influxdb/releases/influxdb-1.2.4.x86_64.rpm

yum localinstall influxdb-1.2.4.x86_64.rpm

systemctl enable influxdb

systemctl start influxdb

curl -POST "http://localhost:8086/query" --data-urlencode "q=CREATE DATABASE mydb"

curl -GET "http://localhost:8086/query?pretty=true" --data-urlencode "q=show databases"

curl -GET 'http://localhost:8086/query?pretty=true' --data-urlencode "db=mydb" --data-urlencode "q=drop measurement myflows"

curl -i -XPOST "http://localhost:8086/write?db=mydb" --data-binary 'myflows,host=host-4,ipv4_src=1.1.1.1,port_src=1,ipv4_dst=2.2.2.2,port_dst=2 protocol="TCP",tcp_flags="RST",total_bytes=10000,total_packets=1010,time_start=1000000,time_end=2222222222'


