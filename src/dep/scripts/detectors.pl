#!/usr/bin/perl -w
#
# Generate the baked-in Fightcade game-detector tables.
#
# Detector rules are COMPILED INTO the executable, not read from disk: LoadMemoryBufferDetector()
# in src/intf/video/win32/vid_detector.cpp only touches the filesystem when force_disk is set, and
# that is debug_mode, which is false in normal play. Without these two headers the lookup table
# contains nothing but its terminator, every game silently gets ST_NONE, and round counters and
# ranked match reporting stop working with no error of any kind.
#
# Writes two files next to each other:
#
#   detector_buffers.h   the file bytes, one array each, plus the FBNEO_DETECTORS_GENERATED
#                        sentinel that vid_detector.cpp checks
#   detector_loaders.h   one ITEM(name, buffer, size) per detector; this one is #included INSIDE
#                        an array initialiser, so it may contain nothing else
#
# Usage:
#   perl detectors.pl -o <outdir> [-r <rev>] <detectors-repo>
#
#   -o   directory to write both headers into
#   -r   git revision to read (default b9e0e40 - see docs/DETECTORS.md for why this pin).
#        Pass -r WORKTREE to read the working tree instead.
#
# *** Bytes are taken from git, not from the working tree, and that is load-bearing. ***
#
# The detectors repository has MIXED line endings in its committed blobs (sfa2u.inf is LF,
# whp.inf is CRLF) and Fightcade's binary contains each file exactly as committed. A Windows
# checkout with core.autocrlf=true rewrites the LF blobs to CRLF on disk, so generating from the
# working tree produces a binary that is subtly NOT byte-identical to Fightcade's. Reading blobs
# with `git cat-file` sidesteps that no matter how the clone was configured, and makes the
# revision pin free.

use strict;

my $Outdir;
my $Rev = "b9e0e40";
my $Repo;

# Process command line arguments (same shape as the other scripts in this directory)
for ( my $i = 0; $i < scalar @ARGV; $i++ ) {

	if ( $ARGV[$i] =~ /^-o/i ) {
		if ( $ARGV[$i] =~ /^-o$/i ) {
			$i++;
			$Outdir = $ARGV[$i] if $i < scalar @ARGV;
		} else {
			$ARGV[$i] =~ /(?<=-o)(.*)/i;
			$Outdir = $1;
		}
		next;
	}

	if ( $ARGV[$i] =~ /^-r/i ) {
		if ( $ARGV[$i] =~ /^-r$/i ) {
			$i++;
			$Rev = $ARGV[$i] if $i < scalar @ARGV;
		} else {
			$ARGV[$i] =~ /(?<=-r)(.*)/i;
			$Rev = $1;
		}
		next;
	}

	$Repo = $ARGV[$i];
}

if ( !defined $Outdir ) {
	die "usage: detectors.pl -o <outdir> [-r <rev>] [detectors-repo]\n";
}

# Locate the detectors clone. An explicit path always wins; otherwise try the layouts people
# actually use, relative to the repository root (this script lives in <root>/src/dep/scripts).
my @tried;
if ( defined $Repo && length $Repo ) {
	@tried = ( $Repo );
} else {
	# Directory holding this script. When invoked as plain "perl detectors.pl" there is no
	# directory component at all, in which case the script's directory IS the cwd.
	my $root = $0;
	$root = "." unless $root =~ s![\\/][^\\/]+$!! && length $root;
	$root .= "/../../..";			# <root>/src/dep/scripts -> repository root

	@tried = (
		"$root/../fightcade-detectors",			# sibling of the repo
		"$root/../../fightcadeorg/fightcade-detectors",	# grouped by org
		"$root/../fightcade-detectors-main",		# github zip download
		"$root/detectors",				# inside the repo
	);
}

my $found;
for my $cand ( @tried ) {
	if ( -d $cand ) { $found = $cand; last; }
}

if ( !defined $found ) {
	print STDERR "detectors.pl: could not find a fightcade-detectors clone.\n";
	print STDERR "              tried:\n";
	print STDERR "                $_\n" for @tried;
	print STDERR "\n";
	print STDERR "              Fix with either:\n";
	print STDERR "                set FBNEO_DETECTORS=C:\\path\\to\\fightcade-detectors\n";
	print STDERR "                perl detectors.pl -o <outdir> C:\\path\\to\\fightcade-detectors\n";
	print STDERR "\n";
	print STDERR "              git clone https://github.com/fightcadeorg/fightcade-detectors\n";
	print STDERR "              See docs/DETECTORS.md.\n";
	exit 1;
}
$Repo = $found;

# MemoryBuffer::Init() in vid_memorybuffer.h SILENTLY truncates at 2048 bytes. A truncated
# detector would load, parse its surviving lines and then simply never fire - the same invisible
# failure this script exists to prevent - so refuse to emit one.
my $MAX_LEN = 2048;

my %files;   # game => raw bytes

if ( uc($Rev) eq "WORKTREE" ) {
	opendir( my $dh, $Repo ) or die "detectors.pl: cannot read '$Repo': $!\n";
	for my $ent ( readdir $dh ) {
		next unless $ent =~ /^(.+)\.inf$/i;
		my $game = lc $1;
		open( my $fh, "<", "$Repo/$ent" ) or die "detectors.pl: cannot open $ent: $!\n";
		binmode $fh;
		local $/ = undef;
		$files{$game} = <$fh>;
		close $fh;
	}
	closedir $dh;
} else {
	# List the .inf blobs at $Rev, then read each one verbatim.
	my @tree = `git -C "$Repo" ls-tree -r $Rev`;
	die "detectors.pl: 'git ls-tree $Rev' failed in '$Repo'\n" if $? != 0;

	for my $line ( @tree ) {
		chomp $line;
		next unless $line =~ /^\S+\s+blob\s+(\S+)\t(.+)$/;
		my ( $sha, $path ) = ( $1, $2 );
		next unless $path =~ /^(.+)\.inf$/i;
		my $game = lc $1;

		my $data = `git -C "$Repo" cat-file blob $sha`;
		die "detectors.pl: cannot read blob $sha ($path)\n" if $? != 0;
		$files{$game} = $data;
	}
}

die "detectors.pl: no .inf files found in '$Repo' at '$Rev'\n" unless scalar keys %files;

# Sorted, so regenerating produces a stable diff.
my @games = sort keys %files;

open( my $buf, ">", "$Outdir/detector_buffers.h" )
	or die "detectors.pl: cannot write $Outdir/detector_buffers.h: $!\n";
open( my $ldr, ">", "$Outdir/detector_loaders.h" )
	or die "detectors.pl: cannot write $Outdir/detector_loaders.h: $!\n";
binmode $buf;
binmode $ldr;

print $buf "// Generated by src/dep/scripts/detectors.pl - do not edit.\n";
print $buf "// Source: $Repo at $Rev\n";
print $buf "//\n";
print $buf "// vid_detector.cpp checks the sentinel below. Building without it silently disables\n";
print $buf "// every game detector, round counter and ranked match report.\n\n";
print $buf "#define FBNEO_DETECTORS_GENERATED " . scalar(@games) . "\n\n";

print $ldr "// Generated by src/dep/scripts/detectors.pl - do not edit.\n";
print $ldr "// Included INSIDE an array initialiser: ITEM() entries only, nothing else.\n";

for my $game ( @games ) {
	my $data = $files{$game};
	my $len  = length $data;

	die "detectors.pl: $game.inf is $len bytes, over the $MAX_LEN limit MemoryBuffer::Init()\n" .
	    "             truncates at. Raise MemoryBuffer::data[] before adding it.\n"
		if $len > $MAX_LEN;

	next if $len == 0;

	# C identifier. Every current name is already valid; this is future-proofing.
	my $sym = "detector_${game}_inf";
	$sym =~ s/[^A-Za-z0-9_]/_/g;
	$sym = "_$sym" if $sym =~ /^[0-9]/;

	print $buf "static const unsigned char $sym\[$len\] = {";
	for ( my $i = 0; $i < $len; $i++ ) {
		print $buf "\n\t" if ( $i % 16 ) == 0;
		printf $buf "0x%02x,", ord( substr( $data, $i, 1 ) );
	}
	print $buf "\n};\n\n";

	# The key GameDetector::Load() looks up is sprintf("detector\\%s.inf", game), compared with
	# _stricmp - so the backslash matters and the case does not.
	print $ldr "ITEM(\"detector\\\\$game.inf\", $sym, $len)\n";
}

close $buf;
close $ldr;

printf "detectors.pl: %d detectors from %s at %s\n", scalar(@games), $Repo, $Rev;
