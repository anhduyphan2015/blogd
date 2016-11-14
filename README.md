What is Blogd?
-------------
Blogd is **a custom Redis version for building Flat-File Blog** which focus about performance, fast development to help busy developers to build a blogging system.
Blogd is developed for **Golr Engineering Blogging System** 

Demo
-------------
[https://engineering.golr.xyz](https://engineering.golr.xyz)

Features
-------------
* Redis with built-in simple HTTP server.
* Support Mardown language to write posts with minimum effort required.
* Auto creating pagination pages.
* Can handle thousand of requests per seconds with Redis event-loop.

Source code layout
-------------
* contents: contains html templates.
* public: contains static files (css,js,images).
* redis-3.2.5: contains the Redis custom version, written in C.

Dependencies
-------------
<pre>
$ sudo apt-get install libpcre3-dev # For Debian/Ubuntu
$ sudo yum install pcre-devel # For RHEL/CentOS
</pre>

Building Blogd
-------------
It is as simple as:
<pre>
$ cd redis-3.2.5
$ make
</pre>

Fixing build problems with dependencies or cached build options
---------
<pre>
$ cd redis-3.2.5
$ make distclean
</pre>

Running Blogd
-------------
To run Redis with the default configuration just type:
<pre>
$ cd redis-3.2.5
$ ./src/redis-server redis.conf
</pre>

Addition configuration
-------------
Change default configuration in "redis-3.2.5/redis.conf" file
<pre>
content-dir "../contents" # The path of content dir
public-dir "../public" # The path of public dir
per-page 10 # Limit posts per page
reload-content-query "secret-reload" # HTTP query param for reloading content (ex: http://blogd.local/?secret-reload)
markdown-compile 0 # Using or not mardown language in templates
</pre>

Notes
-------------
* Currently Blogd only support for Linux (specially on Ubuntu & Centos)
* It should be run behind Nginx in production for security.
* Blogd rewrite the way Redis handles connections so we cannot use Redis utilities: 
redis-benchmark, redis-cli...
