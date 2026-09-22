# bash completion for sdmsg

_sdmsg()
{
	local cur prev words cword
	_init_completion || return

	case $prev in
		-b|--block-size)
			return
			;;
		-d|--sqlite-db)
			_filedir
			return
			;;
	esac

	if [[ $cur == -* ]]; then
		COMPREPLY=($(compgen -W '--block-size --sqlite-db --test --linear --recursive --auto-mount --gui --verbose --quiet --help --version -b -d -t -l -r -m -v -q -h' -- "$cur"))
		return
	fi

	_filedir
}

complete -F _sdmsg sdmsg
