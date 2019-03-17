Compile the kernel
============

Use `make menuconfig` and select `NIMBLE_PAGE_MANAGEMENT` to make sure the
kernel can be compiled correctly. (Use `/` to search for that option.)

Make sure you have `CONFIG_PAGE_MIGRATION_PROFILE=y` in your .config if you want
to run microbenchmarks. (Use `make menuconfig` to search and enable this option.)


Related information
============

This is the kernel of "Nimble Page Management for Tiered Memory Systems".
Its companion userspace applications and microbenchmarks can be find in
https://github.com/ysarch-lab/nimble_page_management_userspace.

Technical details on the kernel will appear in an article soon: https://normal.zone/blog/2019-01-27-nimble-page-management/.


Linux kernel
============

There are several guides for kernel developers and users. These guides can
be rendered in a number of formats, like HTML and PDF. Please read
Documentation/admin-guide/README.rst first.

In order to build the documentation, use ``make htmldocs`` or
``make pdfdocs``.  The formatted documentation can also be read online at:

    https://www.kernel.org/doc/html/latest/

There are various text files in the Documentation/ subdirectory,
several of them using the Restructured Text markup notation.

Please read the Documentation/process/changes.rst file, as it contains the
requirements for building and running the kernel, and information about
the problems which may result by upgrading your kernel.
