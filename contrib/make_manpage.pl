#!/usr/bin/env perl
#
# Copyright (c) 2015      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# Copyright (c) 2015-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Subroutine to generate a bunch of Fortran declarations and symbols
#

use strict;

use Getopt::Long;

my $package_name;
my $package_version;
my $prte_date;
my $cxx = '1';
my $input;
my $output;
my $help_arg = 0;

&Getopt::Long::Configure("bundling");
my $ok = Getopt::Long::GetOptions("package-name=s" => \$package_name,
                                  "package-version=s" => \$package_version,
                                  "prte-date=s" => \$prte_date,
                                  "cxx!" => \$cxx,
                                  "input=s" => \$input,
                                  "output=s" => \$output);

if ($help_arg || !$ok ||
    !defined($input) ||
    !defined($output) ||
    !defined($package_name) ||
    !defined($package_version) ||
    !defined($prte_date)) {
    print "Usage: $0 --package-name=<package name> --package-version=<package version> --prte-date=<prte date> --input=<input file> --output=<output file> [--nocxx]\n";
    exit(1 - $ok);
}

open(FILE, $input) ||
    die "Can't open $input";
my $file;
$file .= $_
    while(<FILE>);
close(FILE);

$file =~ s/#PACKAGE_NAME#/$package_name/g;
$file =~ s/#PACKAGE_VERSION#/$package_version/g;
$file =~ s/#PRTE_DATE#/$prte_date/g;

if ($cxx == 0) {
    $file =~ s/\n\.SH C\+\+ Syntax.+?\n\.SH/\n\.SH/s;
}

open(FILE, ">$output") ||
    die "Can't open $output";
print FILE $file;
close(FILE);

exit(0);
