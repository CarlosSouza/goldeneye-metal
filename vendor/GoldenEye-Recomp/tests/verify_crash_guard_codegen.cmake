if(NOT DEFINED GENERATED_DIRECTORY OR NOT IS_DIRECTORY "${GENERATED_DIRECTORY}")
    message(FATAL_ERROR
        "Generated GoldenEye directory is missing; rerun rexglue codegen before packaging")
endif()

file(GLOB generated_sources "${GENERATED_DIRECTORY}/ge_recomp.*.cpp")

function(extract_generated_function function_name output_variable)
    set(function_start -1)
    foreach(generated_path IN LISTS generated_sources)
        file(READ "${generated_path}" candidate_source)
        string(FIND "${candidate_source}"
            "DEFINE_REX_FUNC(${function_name})" candidate_start)
        if(NOT candidate_start LESS 0)
            set(generated_source "${candidate_source}")
            set(function_start ${candidate_start})
            break()
        endif()
    endforeach()

    if(function_start LESS 0)
        message(FATAL_ERROR
            "Generated source does not contain ${function_name}")
    endif()

    string(LENGTH "${generated_source}" generated_length)
    math(EXPR tail_length "${generated_length} - ${function_start}")
    string(SUBSTRING "${generated_source}" ${function_start} ${tail_length} function_tail)
    string(FIND "${function_tail}" "\nDEFINE_REX_FUNC(" next_function)
    if(next_function LESS 0)
        set(function_body "${function_tail}")
    else()
        string(SUBSTRING "${function_tail}" 0 ${next_function} function_body)
    endif()
    set(${output_variable} "${function_body}" PARENT_SCOPE)
endfunction()

extract_generated_function("sub_823DFB70" packed_data_body)

string(REGEX MATCHALL
    "ge_skip_packed_data_purecall_(header|value)\\("
    guard_calls "${packed_data_body}")
list(LENGTH guard_calls guard_call_count)
if(NOT guard_call_count EQUAL 2)
    message(FATAL_ERROR
        "sub_823DFB70 must contain exactly two pre-dispatch purecall guards; "
        "rerun rexglue codegen")
endif()

foreach(guard IN ITEMS header value)
    if(guard STREQUAL "header")
        set(return_address "0x823DFBAC")
        set(expected_call
            "ge_skip_packed_data_purecall_header(ctx.r11, ctx.r3, ctx.r31, ctx.r1)")
        set(post_call_guard "ge_guard_packed_data_header(")
    else()
        set(return_address "0x823DFBD4")
        set(expected_call
            "ge_skip_packed_data_purecall_value(ctx.r11, ctx.r3, ctx.r30, ctx.r31, ctx.r1)")
        set(post_call_guard "ge_guard_packed_data_value(")
    endif()

    string(FIND "${packed_data_body}" "${expected_call}" guard_position)
    string(FIND "${packed_data_body}"
        "ctx.lr = ${return_address};" dispatch_position)
    string(FIND "${packed_data_body}" "${post_call_guard}" post_call_guard_position)
    if(guard_position LESS 0 OR dispatch_position LESS 0 OR
       post_call_guard_position LESS 0 OR
       NOT guard_position LESS dispatch_position OR
       NOT dispatch_position LESS post_call_guard_position)
        message(FATAL_ERROR
            "The ${guard} purecall guard has incorrect register wiring or is not "
            "before its original virtual dispatch")
    endif()

    math(EXPR guarded_region_length "${dispatch_position} - ${guard_position}")
    string(SUBSTRING "${packed_data_body}" ${guard_position}
        ${guarded_region_length} guarded_region)
    string(FIND "${guarded_region}" "goto loc_823DFBDC;" epilogue_jump)
    if(epilogue_jump LESS 0)
        message(FATAL_ERROR
            "The ${guard} purecall guard does not use sub_823DFB70's normal epilogue")
    endif()

    math(EXPR dispatch_region_length
        "${post_call_guard_position} - ${dispatch_position}")
    string(SUBSTRING "${packed_data_body}" ${dispatch_position}
        ${dispatch_region_length} dispatch_region)
    set(indirect_call "REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);")
    string(LENGTH "${dispatch_region}" dispatch_region_size)
    string(LENGTH "${indirect_call}" indirect_call_size)
    string(REPLACE "${indirect_call}" "" dispatch_without_indirect
        "${dispatch_region}")
    string(LENGTH "${dispatch_without_indirect}" dispatch_without_indirect_size)
    math(EXPR removed_indirect_size
        "${dispatch_region_size} - ${dispatch_without_indirect_size}")
    if(NOT removed_indirect_size EQUAL indirect_call_size)
        message(FATAL_ERROR
            "The ${guard} purecall guard must retain exactly one original "
            "indirect dispatch")
    endif()
endforeach()

extract_generated_function("sub_823CFC00" cleanup_body)
set(cleanup_enter_call
    "ge_cleanup_callback_enter(ctx.r1, ctx.r26, ctx.r27, ctx.r28, ctx.r29, ctx.r30, ctx.r31, ctx.r3, ctx.r11);")
set(cleanup_leave_call
    "ge_cleanup_callback_leave(ctx.r1, ctx.r26, ctx.r27, ctx.r28, ctx.r29, ctx.r30, ctx.r31);")
string(REGEX MATCHALL "ge_cleanup_callback_enter\\(" cleanup_enter_calls "${cleanup_body}")
string(REGEX MATCHALL "ge_cleanup_callback_leave\\(" cleanup_leave_calls "${cleanup_body}")
list(LENGTH cleanup_enter_calls cleanup_enter_count)
list(LENGTH cleanup_leave_calls cleanup_leave_count)
if(NOT cleanup_enter_count EQUAL 2 OR NOT cleanup_leave_count EQUAL 2)
    message(FATAL_ERROR
        "sub_823CFC00 must preserve state around both cleanup callbacks")
endif()

foreach(return_address IN ITEMS 0x823CFC88 0x823CFCBC)
    if(return_address STREQUAL "0x823CFC88")
        set(first_restored_register_use "ctx.r30.u64 = ctx.r31.u64;")
    else()
        set(first_restored_register_use "ctx.r11.u64 = ctx.r31.u64;")
    endif()

    string(FIND "${cleanup_body}" "ctx.lr = ${return_address};" dispatch_position)
    if(dispatch_position LESS 0)
        message(FATAL_ERROR
            "sub_823CFC00 is missing the cleanup dispatch returning to ${return_address}")
    endif()

    string(SUBSTRING "${cleanup_body}" 0 ${dispatch_position} before_dispatch)
    string(FIND "${before_dispatch}" "${cleanup_enter_call}" enter_position REVERSE)
    if(enter_position LESS 0)
        message(FATAL_ERROR
            "The cleanup callback at ${return_address} has no preceding state snapshot")
    endif()
    math(EXPR pre_dispatch_length "${dispatch_position} - ${enter_position}")
    string(SUBSTRING "${cleanup_body}" ${enter_position}
        ${pre_dispatch_length} pre_dispatch_wrapper)
    string(FIND "${pre_dispatch_wrapper}" "REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);"
        earlier_indirect_position)
    string(FIND "${pre_dispatch_wrapper}" "${cleanup_leave_call}"
        earlier_leave_position)
    if(NOT earlier_indirect_position LESS 0 OR NOT earlier_leave_position LESS 0)
        message(FATAL_ERROR
            "The cleanup callback at ${return_address} must take a fresh snapshot "
            "immediately before its own dispatch")
    endif()

    string(LENGTH "${cleanup_body}" cleanup_body_length)
    math(EXPR after_dispatch_length "${cleanup_body_length} - ${dispatch_position}")
    string(SUBSTRING "${cleanup_body}" ${dispatch_position}
        ${after_dispatch_length} after_dispatch)
    string(FIND "${after_dispatch}" "REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);"
        indirect_position)
    string(FIND "${after_dispatch}" "${cleanup_leave_call}" leave_position)
    string(FIND "${after_dispatch}" "${first_restored_register_use}"
        restored_use_position)
    if(enter_position LESS 0 OR indirect_position LESS 0 OR leave_position LESS 0 OR
       restored_use_position LESS 0 OR
       NOT indirect_position LESS leave_position OR
       NOT leave_position LESS restored_use_position)
        message(FATAL_ERROR
            "The cleanup callback at ${return_address} is not wrapped by the "
            "nonvolatile-register guard before restored state is consumed")
    endif()

    set(indirect_call "REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);")
    string(LENGTH "${indirect_call}" indirect_call_length)
    math(EXPR after_first_indirect
        "${indirect_position} + ${indirect_call_length}")
    math(EXPR remaining_guarded_length
        "${leave_position} - ${after_first_indirect}")
    string(SUBSTRING "${after_dispatch}" ${after_first_indirect}
        ${remaining_guarded_length} remaining_guarded_region)
    string(FIND "${remaining_guarded_region}" "${indirect_call}"
        second_indirect_position)
    if(NOT second_indirect_position LESS 0)
        message(FATAL_ERROR
            "The cleanup callback at ${return_address} must retain exactly one "
            "indirect dispatch")
    endif()
endforeach()

function(verify_audio_callback_guard function_name dispatch_site return_address)
    extract_generated_function("${function_name}" audio_callback_body)
    set(enter_call "ge_audio_callback_enter_${dispatch_site}(")
    set(leave_call "ge_audio_callback_leave(")
    set(indirect_call "REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);")

    string(FIND "${audio_callback_body}" "${enter_call}" enter_position)
    string(FIND "${audio_callback_body}"
        "ctx.lr = ${return_address};" return_position)
    string(LENGTH "${audio_callback_body}" audio_callback_body_length)
    if(return_position LESS 0)
        set(after_return "")
    else()
        math(EXPR after_return_length
            "${audio_callback_body_length} - ${return_position}")
        string(SUBSTRING "${audio_callback_body}" ${return_position}
            ${after_return_length} after_return)
    endif()
    string(FIND "${after_return}" "${indirect_call}" indirect_position)
    string(FIND "${after_return}" "${leave_call}" leave_position)
    if(enter_position LESS 0 OR return_position LESS 0 OR
       indirect_position LESS 0 OR leave_position LESS 0 OR
       NOT enter_position LESS return_position OR
       NOT indirect_position LESS leave_position)
        message(FATAL_ERROR
            "${function_name} callback 0x${dispatch_site} is not wrapped by the "
            "audio nonvolatile-register guard")
    endif()

    math(EXPR before_dispatch_length "${return_position} - ${enter_position}")
    string(SUBSTRING "${audio_callback_body}" ${enter_position}
        ${before_dispatch_length} before_dispatch)
    string(FIND "${before_dispatch}" "${indirect_call}" earlier_indirect)
    string(FIND "${before_dispatch}" "${leave_call}" earlier_leave)
    if(NOT earlier_indirect LESS 0 OR NOT earlier_leave LESS 0)
        message(FATAL_ERROR
            "${function_name} callback 0x${dispatch_site} does not take a fresh "
            "snapshot immediately before dispatch")
    endif()

    math(EXPR guarded_return_length "${leave_position}")
    string(SUBSTRING "${after_return}" 0
        ${guarded_return_length} guarded_return)
    string(LENGTH "${guarded_return}" guarded_return_size)
    string(LENGTH "${indirect_call}" indirect_call_size)
    string(REPLACE "${indirect_call}" "" guarded_without_indirect
        "${guarded_return}")
    string(LENGTH "${guarded_without_indirect}" guarded_without_indirect_size)
    math(EXPR removed_indirect_size
        "${guarded_return_size} - ${guarded_without_indirect_size}")
    if(NOT removed_indirect_size EQUAL indirect_call_size)
        message(FATAL_ERROR
            "${function_name} callback 0x${dispatch_site} must retain exactly one "
            "indirect dispatch before state restoration")
    endif()

    # These hooks are intentionally limited to straight-line return addresses.
    # A branch target at the restore would let a callback-skip or shared join
    # execute leave without the matching enter and consume an outer snapshot.
    string(REPLACE "0x" "" return_site "${return_address}")
    string(TOUPPER "${return_site}" return_site)
    string(FIND "${audio_callback_body}" "loc_${return_site}:" return_label)
    string(FIND "${audio_callback_body}" "goto loc_${return_site};" branch_to_return)
    if(NOT return_label LESS 0 OR NOT branch_to_return LESS 0)
        message(FATAL_ERROR
            "${function_name} callback 0x${dispatch_site} restore address "
            "${return_address} is a control-flow join")
    endif()
endfunction()

verify_audio_callback_guard("sub_823E4B60" "823E4BA8" "0x823E4BAC")
verify_audio_callback_guard("sub_823E4B60" "823E4BDC" "0x823E4BE0")

# Reject stale codegen that still contains any of the unsafe broad family
# hooks. Exactly two enters and two paired leaves are allowed.
set(all_generated_source "")
foreach(generated_path IN LISTS generated_sources)
    file(READ "${generated_path}" generated_source_part)
    string(APPEND all_generated_source "${generated_source_part}")
endforeach()
string(REGEX MATCHALL "ge_audio_callback_enter_[0-9A-F]+\\("
    all_audio_enters "${all_generated_source}")
string(REGEX MATCHALL "ge_audio_callback_leave\\("
    all_audio_leaves "${all_generated_source}")
list(LENGTH all_audio_enters all_audio_enter_count)
list(LENGTH all_audio_leaves all_audio_leave_count)
# Each hook appears once in an extern declaration and once at its call site.
if(NOT all_audio_enter_count EQUAL 4 OR NOT all_audio_leave_count EQUAL 4)
    message(FATAL_ERROR
        "Generated GoldenEye code must contain only the two straight-line "
        "sub_823E4B60 audio callback guards; rerun rexglue codegen")
endif()

# Telemetry must run only after the title has dispatched every staged local-pad
# record, not from the pre-dispatch injection hook.
extract_generated_function("sub_823B2548" input_dispatch_body)
string(FIND "${input_dispatch_body}" "ge_inject_keyboard(" input_injection)
string(FIND "${input_dispatch_body}"
    "if (ctx.cr6.lt) goto loc_823B25E8;" dispatch_loop_back)
string(FIND "${input_dispatch_body}"
    "ge_observe_dispatched_player_input();" dispatched_observation)
string(REGEX MATCHALL "ge_observe_dispatched_player_input\\("
    dispatched_observation_calls "${input_dispatch_body}")
list(LENGTH dispatched_observation_calls dispatched_observation_count)
if(input_injection LESS 0 OR dispatch_loop_back LESS 0 OR
   dispatched_observation LESS 0 OR
   NOT input_injection LESS dispatch_loop_back OR
   NOT dispatch_loop_back LESS dispatched_observation OR
   NOT dispatched_observation_count EQUAL 1)
    message(FATAL_ERROR
        "Player-stuck telemetry must sample exactly once after the title's "
        "local-pad dispatch loop")
endif()

extract_generated_function("sub_823DACE0" child_cleanup_body)
set(child_node_guard
    "ge_guard_cleanup_child_node(ctx.r11, ctx.r30, ctx.r1)")
string(REGEX MATCHALL "ge_guard_cleanup_child_node\\("
    child_node_guard_calls "${child_cleanup_body}")
list(LENGTH child_node_guard_calls child_node_guard_count)
if(NOT child_node_guard_count EQUAL 1)
    message(FATAL_ERROR
        "sub_823DACE0 must contain exactly one child-list node guard")
endif()
set(child_node_current
    "ctx.r3.u64 = ctx.r11.u64;")
set(child_next_load
    "ctx.r31.u64 = REX_LOAD_U32(ctx.r11.u32 + 4);")
string(FIND "${child_cleanup_body}" "${child_node_current}" child_current_position)
string(FIND "${child_cleanup_body}" "${child_node_guard}" child_guard_position)
string(FIND "${child_cleanup_body}" "${child_next_load}" child_load_position)
if(child_current_position LESS 0 OR child_guard_position LESS 0 OR
   child_load_position LESS 0 OR
   NOT child_current_position LESS child_guard_position OR
   NOT child_guard_position LESS child_load_position)
    message(FATAL_ERROR
        "sub_823DACE0 must validate the current child-list node at 0x823DAD08 "
        "before node->next")
endif()
math(EXPR child_guard_region_length
    "${child_load_position} - ${child_guard_position}")
string(SUBSTRING "${child_cleanup_body}" ${child_guard_position}
    ${child_guard_region_length} child_guard_region)
string(FIND "${child_guard_region}" "goto loc_823DAD1C;" child_epilogue_jump)
if(child_epilogue_jump LESS 0)
    message(FATAL_ERROR
        "The corrupt child-list guard must use sub_823DACE0's normal epilogue")
endif()

extract_generated_function("sub_823E4B60" critical_section_wait_body)
string(FIND "${critical_section_wait_body}"
    "ctx.r3.u64 = ctx.r29.u64;" critical_section_argument_position)
string(FIND "${critical_section_wait_body}"
    "ctx.lr = 0x823E4B88;" critical_section_return_position)
string(FIND "${critical_section_wait_body}"
    "__imp__RtlEnterCriticalSection(ctx, base);" critical_section_call_position)
if(critical_section_argument_position LESS 0 OR
   critical_section_return_position LESS 0 OR
   critical_section_call_position LESS 0 OR
   NOT critical_section_argument_position LESS critical_section_return_position OR
   NOT critical_section_return_position LESS critical_section_call_position)
    message(FATAL_ERROR
        "sub_823E4B60 must retain r3 == r29 at the critical-section wait "
        "returning to 0x823E4B88")
endif()

message(STATUS
    "Verified audio/cleanup guards, child-list containment, and lock-wait diagnostic callsite")
