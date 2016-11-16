What is Blogd?
-------------
Blogd is **a custom Redis version for building Flat-File Blog** which focus about performance, fast development to help busy developers to build a blog.
Blogd was first developed for **Golr Engineering Blog**.

Demo
-------------
[https://engineering.golr.xyz](https://engineering.golr.xyz)

Features
-------------
* Simple HTTP server inside Redis.
* Support Markdown language to write posts with minimum effort required.
* Auto creating pagination pages.
* Can handle thousand of requests per seconds with Redis event-loop.

Source code layout
-------------
* contents: contains html templates.
* public: contains static files (css,js,images).
* redis-3.2.5: contains the Redis custom version, written in C.

How Blogd works
---------------
* Blogd will read files in "contents" dir when it started and compile it.
* All compiled content will be saved into Redis by key => value (file_name => compiled_content) and keep it in memory.
* When the client requests a resource (Ex: /2016-11-08-failure-is-not-an-option).
The value (compiled content) of "2016-11-08-failure-is-not-an-option" key will be returned back to the client.

Install dependencies
--------------------
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

Blogd configurations
--------------------
Some new configurations added in "redis-3.2.5/redis.conf" file
<pre>
content-dir "../contents" # The path of content dir
public-dir "../public" # The path of public dir
per-page 10 # Limit posts per page
reload-content-query "secret-reload" # HTTP query param for reloading content (ex: http://blogd.local/?secret-reload=1)
markdown-compile 0 # Using or not mardown language in templates
</pre>

Contents directory
------------------
* errors: contains templates for error pages.
* posts: contains posts templates (naming convention: Y-m-d-post-title).
* layout.tpl: master template for all pages.
* page.tpl: template for home page and pagination pages (when your blog has many posts more than 'per-page' config).
* content_top.tpl, header.tpl, footer.tpl: template for specify area in layout.tpl. 
* post.tpl: template of a posts displayed on home page and pagination pages. (will replace for {{ posts }} in "page.tpl").

Notes
-------------
* Currently Blogd only support for Linux (specially on Ubuntu & Centos)
* It should be run behind Nginx in production for security.
* Blogd rewrite the way Redis handles connections so we cannot use Redis utilities: 
redis-benchmark, redis-cli...
