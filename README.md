php-extensions
==============

Lightweight Program Cache for PHP based on a fork of APC and optimized for CLI and CGI use.

With the open-sourcing and releasing of the OPcache acceletator, and w.e.f. PHP 5.5, its 
inclusion in the PHP core, I have decided to suspend all work on LPC to focus on an equivalent 
OPcache-based accelerator, [MLC-OPcache](https://github.com/TerryE/opcache).  


For various technical reasons, APC just isn't architecturally suited to a file-based cache fork, 
so LPC ended up being a total rewrite, and given this radical approach, LPC was never going to be 
anything other than a vehicle to help my understanding what is involved in developing such a 
file-based cache. Immediately after Zend open-sourced OPcache, I did a code review of the OPcache 
source and realised that it provided a far better code base as a starting point for a file-based 
cache, albeit reusing some of the techniques and code that I developed for LPC.

This repository exists for historic reasons only.
