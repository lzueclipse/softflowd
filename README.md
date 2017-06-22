
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

Only for demo, lots of potential bug to fix.

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


