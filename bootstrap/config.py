#!/usr/bin/env python3

# Copyright (C) Catalyst.Net Ltd 2019
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

"""
Manage dependencies and bootstrap environments for Samba.

Config file for packages and templates.

Author: Joe Guo <joeg@catalyst.net.nz>
"""
import os
from os.path import abspath, dirname, join
HERE = abspath(dirname(__file__))
# output dir for rendered files
OUT = join(HERE, 'dists')


# pkgs with same name in all packaging systems
COMMON = [
    'attr',
    'autoconf',
    'binutils',
    'bison',
    'ccache',
    'curl',
    'gcc',
    'gdb',
    'git',
    'make',
    'perl',
    'psmisc',  # for pstree in test
    'sudo',  # docker images has no sudo by default
    'vim',
    'wget',
]


# define pkgs for all packaging systems in parallel
# make it easier to find missing ones
# use latest ubuntu and fedora as defaults
# deb, rpm, ...
PKGS = [
    # NAME1-dev, NAME2-devel
    ('lmdb-utils', 'lmdb-devel'),
    ('nettle-dev', 'nettle-devel'),
    ('zlib1g-dev', 'zlib-devel'),
    ('libbsd-dev', 'libbsd-devel'),
    ('libaio-dev', 'libaio-devel'),
    ('libarchive-dev', 'libarchive-devel'),
    ('libblkid-dev', 'libblkid-devel'),
    ('libxml2-dev', 'libxml2-devel'),
    ('libcap-dev', 'libpcap-devel'),
    ('libacl1-dev', 'libacl-devel'),
    ('libattr1-dev', 'libattr-devel'),

    # libNAME1-dev, NAME2-devel
    ('libpopt-dev', 'popt-devel'),
    ('libreadline-dev', 'readline-devel'),
    ('libjansson-dev', 'jansson-devel'),
    ('liblmdb-dev', 'lmdb-devel'),
    ('libncurses5-dev', 'ncurses-devel'),
    # NOTE: Debian 7+ or Ubuntu 16.04+
    ('libsystemd-dev', 'systemd-devel'),
    ('libkrb5-dev', 'krb5-devel'),
    ('libldap2-dev', 'openldap-devel'),
    ('libcups2-dev', 'cups-devel'),
    ('libpam0g-dev', 'pam-devel'),
    ('libgpgme11-dev', 'gpgme-devel'),
    # NOTE: Debian 8+ and Ubuntu 14.04+
    ('libgnutls28-dev', 'gnutls-devel'),
    ('libdbus-1-dev', 'dbus-devel'),

    # NAME1, NAME2
    # for debian, locales provide locale support with language packs
    # ubuntu split language packs to language-pack-xx
    # for centos, glibc-common provide locale support with language packs
    # fedora split language packs  to glibc-langpack-xx
    ('locales', 'glibc-common'),  # required for locale
    ('language-pack-en', 'glibc-langpack-en'),  # we need en_US.UTF-8
    ('', 'glibc-locale-source'),  # for localedef
    ('bind9', 'bind'),
    ('bind9utils', 'bind-utils'),
    ('dnsutils', ''),
    ('locate', 'mlocate'),
    ('xsltproc', 'libxslt'),
    ('krb5-kdc', 'krb5-workstation'),
    ('apt-utils', 'yum-utils'),
    ('pkg-config', 'pkgconfig'),
    ('procps', 'procps-ng'),  # required for the free cmd in tests
    ('lsb-core', 'redhat-lsb'),  # we need lsb_relase to show info
    ('', 'rpcgen'),  # required for test
    # refer: https://fedoraproject.org/wiki/Changes/SunRPCRemoval
    ('', 'libtirpc-devel'),  # for <rpc/rpc.h> header on fedora
    ('', 'libnsl2-devel'),  # for <rpcsvc/yp_prot.h> header on fedora

    # python
    ('python-dev', 'python-devel'),
    ('python-gpg', 'python2-gpg'),  # defaults to ubuntu/fedora latest
    ('python-crypto', 'python-crypto'),
    ('python-markdown', 'python-markdown'),
    ('python-dnspython', 'python-dns'),

    ('python3-dev', 'python3-devel'),
    ('python3-gpg', 'python3-gpg'),  # defaults to ubuntu/fedora latest
    ('python3-crypto', 'python3-crypto'),
    ('python3-markdown', 'python3-markdown'),
    ('python3-dnspython', 'python3-dns'),

    ('', 'libsemanage-python'),
    ('', 'policycoreutils-python'),

    # perl
    ('libparse-yapp-perl', 'perl-Parse-Yapp'),
    # not strict equivalents
    ('perl-modules', 'perl-ExtUtils-MakeMaker'),
    ('libjson-perl', 'perl-Test-Base'),

    # misc
    # @ means group for rpm, use fedora as rpm default
    ('build-essential', '@development-tools'),
    ('debhelper', ''),
    # rpm has no pkg for docbook-xml
    ('docbook-xml', 'docbook-dtds'),
    ('docbook-xsl', 'docbook-style-xsl'),
    ('flex', ''),
    ('', 'keyutils-libs-devel'),

]


DEB_PKGS = COMMON + [pkg for pkg, _ in PKGS if pkg]
RPM_PKGS = COMMON + [pkg for _, pkg in PKGS if pkg]


APT_BOOTSTRAP = r"""
#!/bin/bash
set -xueo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get -y update

apt-get -y install \
    {pkgs}

apt-get -y autoremove
apt-get -y autoclean
apt-get -y clean

# uncomment locale
# this file doesn't exist on ubuntu1404 even locales installed
if [ -f /etc/locale.gen ]; then
    sed -i '/^#\s*en_US.UTF-8 UTF-8/s/^#\s*//' /etc/locale.gen
fi

locale-gen

# update /etc/default/locale
update-locale LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8

# set both for safe
echo LC_ALL="en_US.UTF-8" >> /etc/environment
echo LANG="en_US.UTF-8" >> /etc/environment
"""


YUM_BOOTSTRAP = r"""
#!/bin/bash
set -xueo pipefail

yum -y -q update
yum -y -q install epel-release
yum -y -q update

yum -y -q --verbose install \
    {pkgs}

yum clean all

# gen locale
localedef -c -i en_US -f UTF-8 en_US.UTF-8

# no update-locale, diy
# LC_ALL is not valid in this file
echo LANG="en_US.UTF-8" > /etc/locale.conf

# set both for safe
echo LC_ALL="en_US.UTF-8" >> /etc/environment
echo LANG="en_US.UTF-8" >> /etc/environment
"""


DNF_BOOTSTRAP = r"""
#!/bin/bash
set -xueo pipefail

dnf -y -q update

dnf -y -q --verbose install \
    {pkgs}

dnf clean all

# gen locale
localedef -c -i en_US -f UTF-8 en_US.UTF-8

# no update-locale, diy
# LC_ALL is not valid in this file
echo LANG="en_US.UTF-8" > /etc/locale.conf

# set both for safe
echo LC_ALL="en_US.UTF-8" >> /etc/environment
echo LANG="en_US.UTF-8" >> /etc/environment
"""


DOCKERFILE = r"""
FROM {docker_image}

# we will use this image to run ci, these ENV vars are important
ENV CC="ccache gcc"

ADD bootstrap.sh /tmp/bootstrap.sh
# need root permission, do it before USER samba
RUN bash /tmp/bootstrap.sh

# make test can not work with root, so we have to create a new user
RUN useradd -m -s /bin/bash samba && \
    mkdir -p /etc/sudoers.d && \
    echo "samba ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/samba

USER samba
WORKDIR /home/samba
# samba tests rely on this
ENV USER=samba LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8
"""

# Vagrantfile snippet for each dist
VAGRANTFILE_SNIPPET = r"""
    config.vm.define "{name}" do |v|
        v.vm.box = "{vagrant_box}"
        v.vm.hostname = "{name}"
        v.vm.provision :shell, path: "{name}/bootstrap.sh"
    end
"""

# global Vagrantfile with snippets for all dists
VAGRANTFILE_GLOBAL = r"""
Vagrant.configure("2") do |config|
    config.ssh.insert_key = false

{vagrantfile_snippets}

end
"""


DEB_DISTS = {
    'debian7': {
        'docker_image': 'debian:7',
        'vagrant_box': 'debian/wheezy64',
        'replace': {
            'libgnutls28-dev': 'libgnutls-dev',
            'libsystemd-dev': '',  # not available, remove
            'lmdb-utils': '',  # not available, remove
            'liblmdb-dev': '',  # not available, remove
            'python-gpg': 'python-gpgme',
            'python3-gpg': '',  # no python3 gpg pkg available, remove
            'language-pack-en': '',   # included in locales
        }
    },
    'debian8': {
        'docker_image': 'debian:8',
        'vagrant_box': 'debian/jessie64',
        'replace': {
            'python-gpg': 'python-gpgme',
            'python3-gpg': 'python3-gpgme',
            'language-pack-en': '',   # included in locales
        }
    },
    'debian9': {
        'docker_image': 'debian:9',
        'vagrant_box': 'debian/stretch64',
        'replace': {
            'language-pack-en': '',   # included in locales
        }
    },
    'ubuntu1404': {
        'docker_image': 'ubuntu:14.04',
        'vagrant_box': 'ubuntu/trusty64',
        'replace': {
            'libsystemd-dev': '',  # remove
            'libgnutls28-dev': 'libgnutls-dev',
            'python-gpg': 'python-gpgme',
            'python3-gpg': 'python3-gpgme',
            'lmdb-utils': 'lmdb-utils/trusty-backports',
            'liblmdb-dev': 'liblmdb-dev/trusty-backports',
        }
    },
    'ubuntu1604': {
        'docker_image': 'ubuntu:16.04',
        'vagrant_box': 'ubuntu/xenial64',
        'replace': {
            'python-gpg': 'python-gpgme',
            'python3-gpg': 'python3-gpgme',
        }
    },
    'ubuntu1804': {
        'docker_image': 'ubuntu:18.04',
        'vagrant_box': 'ubuntu/bionic64',
    },
}


RPM_DISTS = {
    'centos6': {
        'docker_image': 'centos:6',
        'vagrant_box': 'centos/6',
        'bootstrap': YUM_BOOTSTRAP,
        'replace': {
            'python3-devel': 'python34-devel',
            'python2-gpg': 'pygpgme',
            'python3-gpg': '',  # no python3-gpg yet
            '@development-tools': '"@Development Tools"',  # add quotes
            'glibc-langpack-en': '',  # included in glibc-common
            'glibc-locale-source': '',  # included in glibc-common
            'procps-ng': 'procps',  # centos6 still use old name
            # update perl core modules on centos
            # fix: Can't locate Archive/Tar.pm in @INC
            'perl': 'perl-core',
        }
    },
    'centos7': {
        'docker_image': 'centos:7',
        'vagrant_box': 'centos/7',
        'bootstrap': YUM_BOOTSTRAP,
        'replace': {
            'python3-devel': 'python34-devel',
            # although python36-devel is available
            # after epel-release installed
            # however, all other python3 pkgs are still python34-ish
            'python2-gpg': 'pygpgme',
            'python3-gpg': '',  # no python3-gpg yet
            '@development-tools': '"@Development Tools"',  # add quotes
            'glibc-langpack-en': '',  # included in glibc-common
            'glibc-locale-source': '',  # included in glibc-common
            # update perl core modules on centos
            # fix: Can't locate Archive/Tar.pm in @INC
            'perl': 'perl-core',
        }
    },
    'fedora28': {
        'docker_image': 'fedora:28',
        'vagrant_box': 'fedora/28-cloud-base',
        'bootstrap': DNF_BOOTSTRAP,
    },
    'fedora29': {
        'docker_image': 'fedora:29',
        'vagrant_box': 'fedora/29-cloud-base',
        'bootstrap': DNF_BOOTSTRAP,
    },
}


DEB_FAMILY = {
    'name': 'deb',
    'pkgs': DEB_PKGS,
    'bootstrap': APT_BOOTSTRAP,  # family default
    'dists': DEB_DISTS,
}


RPM_FAMILY = {
    'name': 'rpm',
    'pkgs': RPM_PKGS,
    'bootstrap': YUM_BOOTSTRAP,  # family default
    'dists': RPM_DISTS,
}


YML_HEADER = r"""
---
packages:
"""


def expand_family_dists(family):
    dists = {}
    for name, config in family['dists'].items():
        config = config.copy()
        config['name'] = name
        config['home'] = join(OUT, name)
        config['family'] = family['name']

        # replace dist specific pkgs
        replace = config.get('replace', {})
        pkgs = []
        for pkg in family['pkgs']:
            pkg = replace.get(pkg, pkg)  # replace if exists or get self
            if pkg:
                pkgs.append(pkg)
        pkgs.sort()

        lines = ['  - {}'.format(pkg) for pkg in pkgs]
        config['packages.yml'] = YML_HEADER.lstrip() + os.linesep.join(lines)

        sep = ' \\' + os.linesep + '    '
        config['pkgs'] = sep.join(pkgs)

        # get dist bootstrap template or fall back to family default
        bootstrap_template = config.get('bootstrap', family['bootstrap'])
        config['bootstrap.sh'] = bootstrap_template.format(**config).strip()

        config['Dockerfile'] = DOCKERFILE.format(**config).strip()
        # keep the indent, no strip
        config['vagrantfile_snippet'] = VAGRANTFILE_SNIPPET.format(**config)

        dists[name] = config
    return dists


# expanded config for dists
DEB_DISTS_EXP = expand_family_dists(DEB_FAMILY)
RPM_DISTS_EXP = expand_family_dists(RPM_FAMILY)

# assemble all together
DISTS = {}
DISTS.update(DEB_DISTS_EXP)
DISTS.update(RPM_DISTS_EXP)


def render_vagrantfile(dists):
    """
    Render all snippets for each dist into global Vagrantfile.

    Vagrant supports multiple vms in one Vagrantfile.
    This make it easier to manage the fleet, e.g:

    start all: vagrant up
    start one: vagrant up ubuntu1804

    All other commands apply to above syntax, e.g.: status, destroy, provision
    """
    # sort dists by name and put all vagrantfile snippets together
    snippets = [
        dists[dist]['vagrantfile_snippet']
        for dist in sorted(dists.keys())]

    return VAGRANTFILE_GLOBAL.format(vagrantfile_snippets=''.join(snippets))


VAGRANTFILE = render_vagrantfile(DISTS)


# data we need to expose
__all__ = ['DISTS', 'VAGRANTFILE', 'OUT']
