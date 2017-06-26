
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
Will store data in elasticsearch via HTTP RESTFUL API.

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

#elasticsearch 
----------
1)edit /etc/sysctl.conf

kernel.core_uses_pid = 1

kernel.core_pattern = /esdata/core/core.%e.%p.%h.%t

kernel.msgmnb = 65536

kernel.msgmax = 65536

kernel.shmmax = 68719476736

kernel.shmall = 4294967296

kernel.sem=300  307200  32  1024

fs.file-max = 6553560

2)eidt /etc/sysctl.d/elasticsearch.conf
vm.max_map_count=262144

3)edit /etc/security/limits.conf

* soft nofile 655350
* hard nofile 655350
* soft core  unlimited

4)yum install java-1.8.0-openjdk-devel

5)edit /etc/yum.repos.d/es.repo
[elasticsearch-5.x]
name=Elasticsearch repository for 5.x packages
baseurl=https://artifacts.elastic.co/packages/5.x/yum
gpgcheck=1
gpgkey=https://artifacts.elastic.co/GPG-KEY-elasticsearch
enabled=1
autorefresh=1
type=rpm-md

6)yum install elasticsearch

7)edit /etc/elasticsearch/elasticsearch.yml
cluster.name: my-es
node.name: host-4
path.data: /esdata/data
path.logs: /esdata/logs
network.host: 10.0.0.4
http.port: 9200

8)edit /etc/elasticsearch/jvm.options
-Xms12g
-Xmx12g

9)
mkdir -p /esdata/data
mkdir -p /esdata/logs
chown -R elasticsearch:elasticsearch /esdata

10)
systemctl daemon-reload
systemctl enable elasticsearch.service
systemctl start elasticsearch.service
systemctl stop elasticsearch.service
systemctl restart elasticsearch.service

curl -XGET 'http://localhost:9200/_cluster/state?pretty=true'



