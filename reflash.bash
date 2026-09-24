# bash completion for reflash

_reflash()
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
		COMPREPLY=($(compgen -W '--block-size --sqlite-db --test --linear --recursive --gui --verbose --quiet --help --version -b -d -t -l -r -g -v -q -h' -- "$cur"))
		return
	fi

	_filedir
}

complete -F _reflash reflash
