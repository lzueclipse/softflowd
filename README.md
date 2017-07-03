
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

softflowd.c "insert_to_influxdb"

Will store data in elasticsearch via HTTP RESTFUL API.

softflowd.c "insert_to_elasticsearch"

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

\* soft nofile 655350

\* hard nofile 655350

\* soft core  unlimited

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

9)mkdir -p /esdata/data

mkdir -p /esdata/logs

chown -R elasticsearch:elasticsearch /esdata

10)systemctl daemon-reload

systemctl enable elasticsearch.service

systemctl start elasticsearch.service

systemctl stop elasticsearch.service

systemctl restart elasticsearch.service

curl -XGET 'http://localhost:9200/_cluster/state?pretty=true'


11)Integration with softflowd

curl -XDELETE 'localhost:9200/my_index?pretty'

curl -XPUT 'localhost:9200/my_index?pretty' -H 'Content-Type: application/json' -d'
{
	
	"settings" : {
        
		"index" : {
            
		"number_of_shards" : 5, 
        
	    	"number_of_replicas" : 1 
        
		}
    	
	},
	
	"mappings" : {
	
		"my_flows" :{
	
			"properties" : {
				
				"@timestamp":   { "index": "not_analyzed", "type": "date" },
		
				"agent_host_name": { "index": "analyzed", "type": "text" },
		
				"ipv4_dst_addr": { "index": "analyzed", "type": "ip" },
		
				"ipv4_src_addr": { "index": "analyzed", "type": "ip" },
				
				"l4_dst_port": { "index": "not_analyzed", "type": "long" },

				"l4_src_port": { "index": "not_analyzed", "type": "long" },

				"tcp_flags": { "index": "not_analyzed", "type": "long" },
				
				"tcp_flags_text": { "index": "not_analyzed", "type": "text" },
				
				"has_tcp_rst": { "index": "not_analyzed", "type": "long" },

				"protocol": { "index": "not_analyzed", "type": "long" },
				
				"protocol_text": { "index": "not_analyzed", "type": "text" },
				
				"first_switched": { "index": "not_analyzed", "type": "date"},
				
				"first_switched_text": { "index": "not_analyzed", "type": "text"},

				"last_switched": { "index": "not_analyzed", "type": "date"},
				
				"last_switched_text": { "index": "not_analyzed", "type": "text"},

				"in_bytes": { "index": "not_analyzed", "type": "long" },

				"in_pkts": { "index": "not_analyzed", "type": "long" }


			}

		}

	}
}
'

curl -XGET 'localhost:9200/my_index/_settings,_mappings?pretty'

curl -XGET 'localhost:9200/my_index/my_flows/_search?q=*&sort=ipv4_src_addr:asc&pretty&pretty'

curl -XGET 'localhost:9200/my_index/my_flows/_search?pretty' -H 'Content-Type: application/json' -d'

{

  "query": { "match_all": {} },

  "sort": [

    { "last_switched": "asc" }

  ]
}
'

curl -XPOST 'localhost:9200/my_index/my_flows/_delete_by_query?pretty' -H 'Content-Type: application/json' -d'

{

  "query": { 

    "match": {

      "ipv4_src_addr": "1.1.1.1"

    }

  }

}
'

curl -XGET 'localhost:9200/my_index/my_flows/_search?pretty' -H 'Content-Type: application/json' -d'

{

  "query": {

    "bool": {

      "should": [

        { "match": { "first_switched": "1498624757" } },

        { "match": { "last_switched": "1498624762" } }

      ]

    }

  }

}'

curl -XGET 'localhost:9200/my_index/my_flows/_search?pretty' -H 'Content-Type: application/json' -d'

{

  "query": {

    "bool": {

      "must": [

        { "match": { "first_switched": "1498624757" } },

        { "match": { "last_switched": "1498624762" } }

      ]

    }

  }

}'

curl -XGET 'localhost:9200/my_index/my_flows/_search?pretty' -H 'Content-Type: application/json' -d'
{
  "query": { "match": { "has_tcp_rst": 1 } }
}
'

