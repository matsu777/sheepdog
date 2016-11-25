Sheepdog Project
================

Sheepdog is a distributed object storage system for volume and container
services and manages the disks and nodes intelligently. Sheepdog features
ease of use, simplicity of code and can scale out to thousands of nodes.

- The block level volume abstraction can be attached to
  `QEMU <http://www.qemu.org/>`__ virtual machines and
  `Linux SCSI Target <http://stgt.sourceforge.net/>`__ and supports
  advanced volume management features such as snapshot, cloning, and
  thin provisioning.

- The object level container abstraction is designed to be
  `Openstack Swift <https://wiki.openstack.org/wiki/Swift>`__ and
  `Amazon S3 <http://aws.amazon.com/s3/>`__ API compatible and can be used to
  store and retrieve any amount of data with a simple web services
  interface.

.. figure:: overview.png
   :alt: figure of sheepdog overview
   :align: center
   :scale: 80

Documentations
--------------

.. toctree::
   :maxdepth: 1

* Wiki

  + https://github.com/sheepdog/sheepdog/wiki

+ Presentations

  + Sheepdog: Yet Another All-In-One Storage For Openstack, Openstack Hong Kong Summit, Nov 2013. (`Slides <_static/sheepdog-openstack.pdf>`__)
  + Sheepdog: Distributed Storage System for QEMU, KVM Forum 2010, Aug 2010. (`Slides <_static/kvmforum2010.pdf>`__)
  + Sheepdog: Distributed Storage System for QEMU/KVM, LCA 2010 DS&R miniconf, Jan 2010. (`Slides <_static/lca2010_miniconf.pdf>`__)

Source Code
-----------
Sheepdog is an Open Source software, released under the terms of the
`GPL2 <./_static/LICENSE.txt>`__.

+ The latest 1.x release is `1.0 <https://github.com/sheepdog/sheepdog/tarball/v1.0>`__
+ The latest 0.9.x release is `0.9.3 <https://github.com/sheepdog/sheepdog/tarball/v0.9.3>`__
+ The latest 0.8.x release is `0.8.3 <https://github.com/sheepdog/sheepdog/tarball/v0.8.3>`__
+ The latest 0.7.x release is `0.7.8 <https://github.com/sheepdog/sheepdog/tarball/v0.7.8>`__
+ The latest developent code is available on the git tree

  + server: git://github.com/sheepdog/sheepdog.git
    [`browse <https://github.com/sheepdog/sheepdog>`__]
  + client: git://git.qemu.org/qemu.git
    [`browse <http://git.qemu.org/qemu.git>`__]

Packages
--------
You can also install Sheepdog with deb/rpm packages.
Before installing Sheepdog packages, install dependent packages listed at Getting Started on Wiki.

+ v1.0.1
  + `Ubuntu 16.04 <https://github.com/sheepdog/sheepdog/raw/gh-pages/data/package/v1.0.1/ubuntu16.04/sheepdog-stable_1.0-1_amd64.deb>`__
  + `CentOS 7 <https://github.com/sheepdog/sheepdog/raw/gh-pages/data/package/v1.0.1/cenots7/sheepdog-1.0.0-1.el7.centos.x86_64.rpm>`__

+ v1.0.0
  + `Ubuntu 16.04 <https://github.com/sheepdog/sheepdog/raw/gh-pages/data/package/v1.0.0/ubuntu16.04/sheepdog-1.0.0-1.amd64.deb>`__
  + `CentOS 7 <https://github.com/sheepdog/sheepdog/raw/gh-pages/data/package/v1.0.0/cenots7/sheepdog-1.0.0-1.el7.centos.x86_64.rpm>`__

Mailing list and IRC
--------------------

+ Developers Mailing list

  + Subscribe: `http://lists.wpkg.org/mailman/listinfo/sheepdog
    <http://lists.wpkg.org/mailman/listinfo/sheepdog>`__
  + Archive:  `http://lists.wpkg.org/pipermail/sheepdog/
    <http://lists.wpkg.org/pipermail/sheepdog/>`__

+ Users Mailing list

  + Subscribe: `http://lists.wpkg.org/mailman/listinfo/sheepdog-users
    <http://lists.wpkg.org/mailman/listinfo/sheepdog-users>`__
  + Archive:  `http://lists.wpkg.org/pipermail/sheepdog-users/
    <http://lists.wpkg.org/pipermail/sheepdog-users/>`__

+ IRC

  + #sheepdog on `freenode <http://webchat.freenode.net/>`__
