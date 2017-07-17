[![Build Status](https://travis-ci.org/rpodgorny/unionfs-fuse.svg?branch=master)](https://travis-ci.org/rpodgorny/unionfs-fuse)
[![Gratipay](http://img.shields.io/gratipay/rpodgorny.svg)](https://gratipay.com/rpodgorny/)

unionfs-fuse
============

This is my effort to create a unionfs filesystem implementation which is way more
flexible than the current in-kernel unionfs solution.

I'm open to patches, suggestions, whatever...

The preferred way is the mailing list at unionfs-fuse@googlegroups.com
or see http://groups.google.com/group/unionfs-fuse.

Why choose this stuff
---------------------

* The filesystem has to be mounted after the roots are mounted when using the standard module. With unionfs-fuse, you can mount the roots later and their contents will appear seamlesly
* You get caching which speeds things up a lot for free
* Advanced features like copy-on-write and more

Why NOT choose it
-----------------

* Compared to kernel-space solution we need lots of useless context switches which makes kernel-only solution clear speed-winner (well, actually I've made some tests and the hard-drives seem to be the bottleneck so the speed is fine, too)

Note for THIS fork
==================

This is a fork from original repository: https://github.com/rpodgorny/unionfs-fuse and COWOLF feature is implmented here.

When new data is written in an existing file located in lower read-only branch, the whole file is copied from lower branch to upper read-write branch. This is known as COPY-UP operation in unionfs-fuse. For example, if there is a file 'foo' with size 1MB in lower read-only branch, this is how it would look like BEFORE and AFTER write operation on this file.

'''
BEEFORE any write() on foo:

upper_branch (read-write):        no foo file here

lower_branch (read-only):         -----------------------------------------------------------
                                  |DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD|
                                  -----------------------------------------------------------
                                                                    
AFTER write(size=1K, offset=2K) on foo:

                                   ---2K----->|<--1K-->|
lower_branch (read-write):        -----------------------------------------------------------
                                  |DDDDDDDDDDD|NNNNNNNN|DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD|
                                  -----------------------------------------------------------

lower_branch (read-only):         -----------------------------------------------------------
                                  |DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD|
                                  -----------------------------------------------------------
'''

Here, existing data is marked with "D" and modified data (due to write) is marked with "N"

This works perfectly as long a the files are small and COPY-UP is quick. But COPY-UP becomes problematic for very large files when it can take very long time to copy file lower branch to upper branch. For example, very large database file or virtual machine disk image.

COWOLF feature solves this problem by NOT copying the whole file. Instead doing COPY-UP the whole file, it creates a sparse file on upper branch. It also creates a map-table in metadata directory to keep track of modified data (due to write operations) on upper branch. New data is written into sparse file and corresponding <offset, length> entry is added in map table. If a read request falls within a <offset, length> range of a map entry, data is served from the upper branch. Otherwise, data is read from lower branch.

With this feature, the above diagram would look like this:
'''
BEEFORE any write() on foo:

upper_branch (read-write):        no foo file here

lower_branch (read-only):         -----------------------------------------------------------
                                  |DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD|
                                  -----------------------------------------------------------
                                         
AFTER write(size=1K, offset=2K) on foo:

                                   ---2K----->|<--1K-->|
lower_branch (read-write):        -----------------------------------------------------------
                                  |  sparse   |NNNNNNNN|       sparse                       |
                                  -----------------------------------------------------------

lower_branch (read-only):         -----------------------------------------------------------
                                  |DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD|
                                  -----------------------------------------------------------
'''

For newly written data marked with "N", an entry is added in map table.
For a read(size=4K, offset=2K) on this file, first 1K is served from upper branch and next 3K is served from lower branch.

This feature sits on the top of "cow" feature (i.e. if cow feature is not enabled, this large file optimization won't be enabled either). An additional option "cowolf" (copy-on-write optimized for large file) is required to enable this feature. Another option "cowolf_file_size" is introduced to specify the minimum threshold size to trigger cowolf feature. The default value is 100MB. File size lower than 100MB is copied from lower branch to upper branch using as usual COW feature. If file is bigger than 100MB, cowolf feature kicks in.

For example, the following option enables COWOLF and this feature kicks in for any file larger than 64k size.

-o cow,cowolf,cowolf_file_size=64k

Currently this feature works for only two branches (read-only lower branch and read-write upper branch).
