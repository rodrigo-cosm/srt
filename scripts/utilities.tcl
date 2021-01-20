# Utility. Allows to put line-oriented comments and have empty lines
proc no_comments {input} {
	set output ""
	foreach line [split $input \n] {
		set nn [string trim $line]
		if { $nn == "" || [string index $nn 0] == "#" } {
			continue
		}
		append output $line\n
	}

	return $output
}

proc screamcase {sym} {
	set out [string toupper $sym]

	set out [string map {- _ + X} $out]

	return $out
}
