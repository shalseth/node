set pagination off
set confirm off
handle SIGILL nostop noprint pass
set $growth_calls = 0
break v8::internal::NativeRegExpMacroAssembler::GrowStack
commands
silent
set $growth_calls = $growth_calls + 1
continue
end
run
printf "REGEXP_STACK_GROWTH_CALLS=%d\n", $growth_calls
printf "REGEXP_STACK_PROCESS_EXIT=%d\n", $_exitcode
