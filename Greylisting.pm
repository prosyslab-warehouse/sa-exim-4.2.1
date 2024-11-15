package Greylisting;
#
# $Id: Greylisting.pm,v 1.4 2006/01/11 17:17:28 marcmerlin Exp $
#

# General Greylisting Plugin, written by Marc MERLIN <marc_soft@merlins.org>
# (Kristopher Austin gets the credit for the original port to an SA 3.0 plugin)
#
# This was originally written to implement greylisting in SA-Exim, although
# I have tried to make it more general and allow for reuse in other MTAs
# (although they will need to
# 1) be running SA at SMTP time
# 2) Provide the list of rcpt to and env from in some headers for SA to read
# 3) Provide the IP of the connecting host )
#
# This rule should get a negative score so that if we've already seen the
# greylisting tuplet before, we lower the score, which hopefully brings us from
# a tempreject to an accept (at least that's how sa-exim does it)
# 
# -- Marc 2004/01/19

use strict;
use Mail::SpamAssassin::Plugin;
use Mail::SpamAssassin::Util qw(untaint_var);
use NetAddr::IP;
use File::Path qw(mkpath);
our @ISA = qw(Mail::SpamAssassin::Plugin);

sub new 
{
    my ($class, $mailsa) = @_;
    $class = ref($class) || $class;
    my $self = $class->SUPER::new($mailsa);
    bless ($self, $class);
    $self->register_eval_rule ("greylisting");
    return $self;
}


sub check_end
{
    my ($self, $permsgstatus) = @_;

    if (not $self->{'rangreylisting'})
    {
	Mail::SpamAssassin::Plugin::dbg("GREYLISTING: greylisting didn't run since the configuration wasn't setup to call us");
    }
}

# Greylisting happens depending on the SA score, so we want to run it last,
# which is why we give it a high priority
sub greylisting 
{
    my ($self, $permsgstatus, $optionhash) = @_;

    my $connectip; 
    my $envfrom;
    my $rcptto;
    my @rcptto;
    my $iswhitelisted=0;
    my $err;
    my $mesgid = $permsgstatus->get('Message-Id')."\n"; 
    my $mesgidfn;
    my $tuplet;
    my $sascore = $permsgstatus->get_score();
    my $dontcheckscore;
    my %option;

    if ($self->{main}->{lint_rules}) {
       Mail::SpamAssassin::Plugin::dbg("GREYLISTING: disabled while linting");
       return 0;
    }
    Mail::SpamAssassin::Plugin::dbg("GREYLISTING: called function");

    while ($optionhash =~ /(?:\G(?<!^)|^\s*\()\s*(?>(?<quot1>['"])(?<opt>.*?)\g{quot1})
	   \s*=>\s*
	   (?>(?<quot2>['"])(?<val>.*?)\g{quot2}
	      |
	      (?<val>-?(?:\d+(?:\.\d*)?|(?:\d*\.)?\d+))
	   )\s*(?:;?\s*\)\s*$|;(?!$))/gxc) {
	$option{$+{opt}} = untaint_var($+{val});
    }
    if ((pos($optionhash) // 0) < length $optionhash) {
	die "Syntax error";
    }
    $self->{'rangreylisting'}=1;

    foreach my $reqoption (qw ( method greylistsecs dontgreylistthreshold
	connectiphdr envfromhdr rcpttohdr greylistnullfrom greylistfourthbyte ))
    {
	die "Greylist option $reqoption missing from SA config" unless (defined $option{$reqoption});
    }

    $dontcheckscore = $option{'dontgreylistthreshold'};


    # No newlines, thank you (yes, you need this twice apparently)
    chomp ($mesgid);
    chomp ($mesgid);
    # Newline in the middle mesgids, are you serious? Get rid of them here
    $mesgid =~ s/\012/|/g;

    # For stuff that we know is spam, don't greylist the host
    # (that might help later spam with a lower score to come in)
    if ($sascore >= $dontcheckscore)
    {
	Mail::SpamAssassin::Plugin::dbg("GREYLISTING: skipping greylisting on $mesgid, since score is already $sascore and you configured greylisting not to bother with anything above $dontcheckscore");
	return 0;
    }
    else
    {
	Mail::SpamAssassin::Plugin::dbg("GREYLISTING: running greylisting on $mesgid, since score is too low ($sascore) and you configured greylisting to greylist anything under $dontcheckscore");
    }

    if (not $connectip = $permsgstatus->get($option{'connectiphdr'}))
    {
	warn "Couldn't get Connecting IP header $option{'connectiphdr'} for message $mesgid, skipping greylisting call\n";
	return 0;
    }
    chomp($connectip);
    # Clean up input (for security, if you use files/dirs)

    $connectip = NetAddr::IP->new($connectip);
    if (not defined $connectip) {
	warn "Can only handle IPv4 and IPv6 addresses; skipping greylisting call for message $mesgid\n";
	return 0;
    }

    # Account for a null envelope from
    if (not defined ($envfrom = $permsgstatus->get($option{'envfromhdr'})))
    {
	warn "Couldn't get Envelope From header $option{'envfromhdr'} for message $mesgid, skipping greylisting call\n";
	return 0;
    }
    chomp($envfrom);
    # Clean up input (for security, if you use files/dirs)
    $envfrom =~ s#/#-#g;
    if (not $envfrom)
    {
	$envfrom="<>";
	return 0 if (not $option{'greylistnullfrom'});
    }

    if (not $rcptto = $permsgstatus->get($option{'rcpttohdr'}))
    {
	warn "Couldn't get Rcpt To header $option{'rcpttohdr'} for message $mesgid, skipping greylisting call\n";
	return 0;
    }
    chomp($rcptto);
    # Clean up input (for security, if you use files/dirs)
    $rcptto =~ s#/#-#g;
    @rcptto = split(/, /, $rcptto);


    umask 0007;

    foreach $rcptto (@rcptto)
    {
	# The dir method is easy to fiddle with and expire records in (with
	# a find | rm) but it's probably more I/O extensive than a real DB
	# and suffers from directory size problems if a specific IP is sending
	# generating tens of thousands of tuplets. -- Marc
	# That said, I prefer formats I can easily tinker with, and not having
	# to worry about buggy locking and so forth

	if ($option{'method'} eq "dir")
	{
	    my $tmpvar;

	    # The clean strings are hardcoded because it's hard to do a variable
	    # substitution within a tr (and using the eval solution is too
	    # resource expensive)
	    # envfrom could be cleaned outside of the loop, but the other method 
            # options might now want that
	    $envfrom =~ tr/!#%()*+,-.0123456789:<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]^_abcdefghijklmnopqrstuvwxyz{|}~/_/c;
	    # clean variables to run properly under -T
	    $envfrom =~ /(.+)/;
	    $tmpvar = ($1 or "");
	    # work around bug in perl untaint in perl 5.8
	    $envfrom=undef;
	    $envfrom=$tmpvar;
	    $envfrom =~ s/^([a-z0-9._]*)[^@]*/$1/i;

	    $rcptto  =~ tr/!#%()*+,-.0123456789:<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]^_abcdefghijklmnopqrstuvwxyz{|}~/_/c;
	    $rcptto =~ /(.+)/;
	    $tmpvar = ($1 or "");
	    $rcptto=undef;
	    $rcptto=$tmpvar;

	    die "greylist option dir not passed, even though method was set to dir" unless ($option{'dir'});
	    
	    # connectip is supposed to be untainted now, but I was still getting
	    # some insecure dependecy error messages sometimes (perl 5.8 problem apparently)
	    my $ipdir;
	    if ($connectip->version == 6) {
		my @components = split ':', $connectip->full, 5;
		if ($option{'greylistfourthbyte'}) {
		    $ipdir = join '/', @components;
		} else {
		    $ipdir = join '/', @components[0..3];
		}
	    } else {
		my @components = split '\.', $connectip->addr;
		if ($option{'greylistfourthbyte'}) {
		    $ipdir = join '/', @components;
		} else {
		    $ipdir = join '/', @components[0..2];
		}
	    }
	    my $tupletdir = "$option{'dir'}/$ipdir/$envfrom";
	    $tuplet = "$tupletdir/$rcptto";

	    # make directory whether it's there or not (faster than test and set)
	    mkpath $tupletdir;

	    if (not -e $tuplet) 
	    {
		# If the tuplets aren't there, we create them and continue in
		# case there are other ones (one of them might be whitelisted
		# already)
		$err="creating $tuplet";
		open (TUPLET, ">$tuplet") or goto greylisterror;
		print TUPLET time."\n";
		print TUPLET "Status: Greylisted\n";
		print TUPLET "Last Message-Id: $mesgid\n";
		print TUPLET "Whitelisted Count: 0\n";
		print TUPLET "Query Count: 1\n";
		print TUPLET "SA Score: $sascore\n";
		$err="closing first-written $tuplet";
		close TUPLET or goto greylisterror;
	    }
	    else
	    {
		my $time;
		my $status;
		my $whitelistcount;
		my $querycount;

		# Take into account race condition of expiring deletes and us
		# running
		$err="reading $tuplet";
		open (TUPLET, "<$tuplet") or goto greylisterror;
		$err="Couldn't read time";
		defined ($time=<TUPLET>) or goto greylisterror;
		chomp ($time);

		$err="Couldn't read status";
		defined ($status=<TUPLET>) or goto greylisterror;
		chomp ($status);
		$err="Couldn't extract Status from $status";
		$status =~ s/^Status: // or goto greylisterror;

		# Skip Mesg-Id
		$err="Couldn't skip Mesg-Id";
		defined ($_=<TUPLET>) or goto greylisterror;

		$err="Couldn't read whitelistcount";
		defined ($whitelistcount=<TUPLET>) or goto greylisterror;
		chomp ($whitelistcount);
		$err="Couldn't extract Whitelisted Count from $whitelistcount";
		$whitelistcount =~ s/^Whitelisted Count: // or goto greylisterror;

		$err="Couldn't read querycount";
		defined ($querycount=<TUPLET>) or goto greylisterror;
		chomp ($querycount);
		$err="Couldn't extract Query Count from $querycount";
		$querycount =~ s/^Query Count: // or goto greylisterror;
		close (TUPLET);

		$querycount++;
		if ((time - $time) > $option{'greylistsecs'})
		{
		    $status="Whitelisted";
		    $whitelistcount++;
		}

		$err="re-writing $tuplet";
		open (TUPLET, ">$tuplet") or goto greylisterror;
		print TUPLET "$time\n";
		print TUPLET "Status: $status\n";
		print TUPLET "Last Message-Id: $mesgid\n";
		print TUPLET "Whitelisted Count: $whitelistcount\n";
		print TUPLET "Query Count: $querycount\n";
		print TUPLET "SA Score: $sascore\n";
		$err="closing re-written $tuplet";
		close TUPLET or goto greylisterror;

		# We continue processing the other recipients, to setup or
		# update their counters
		if ($status eq "Whitelisted")
		{
		    $iswhitelisted=1;
		}
	    }
	}
	elsif ($option{'method'} eq "file")
	{
	    warn "codeme (file greylisting)\n";
	}
	elsif ($option{'method'} eq "db")
	{
	    warn "codeme (db greylisting)\n";
	}
    }
    
    Mail::SpamAssassin::Plugin::dbg("GREYLISTING: computed greylisting on tuplet, saved info in $tuplet and whitelist status is $iswhitelisted");
    return $iswhitelisted;
    
    greylisterror:
    warn "Reached greylisterror: $err / $!";
    # delete tuplet since it apparently had issues but don't check for errors
    # in case it was a permission denied on write
    unlink ($tuplet);
    return $iswhitelisted;
}


1;
