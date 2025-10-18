-- debug_utils.lua - 调试工具函数模块
-- 提供用于打印Rime对象详细信息的工具函数
local debug_utils = {}

-- 打印Candidate的详细信息
function debug_utils.print_candidate_info(cand, index, logger)
    if not cand then
        logger.debug("  candidate is nil")
        return
    end

    logger.debug(string.format("  候选项 %d:", index))
    logger.debug("    type: " .. tostring(cand.type))
    logger.debug("    start: " .. tostring(cand.start))
    logger.debug("    _end: " .. tostring(cand._end))
    logger.debug("    text: '" .. (cand.text or "") .. "'")
    logger.debug("    comment: '" .. (cand.comment or "") .. "'")
    logger.debug("    preedit: '" .. (cand.preedit or "") .. "'")
    logger.debug("    quality: " .. tostring(cand.quality))
end

-- 打印Segment的详细信息（针对翻译器）
function debug_utils.print_segment_info(seg, logger)
    if not seg then
        logger.debug("  segment is nil")
        return
    end

    logger.debug("=== Segment 信息 ===")
    logger.debug("  status: " .. tostring(seg.status))
    logger.debug("  start: " .. tostring(seg.start))
    logger.debug("  _start: " .. tostring(seg._start))
    logger.debug("  _end: " .. tostring(seg._end))
    logger.debug("  length: " .. tostring(seg.length))

    -- tags信息
    logger.debug("  tags:")
    if seg.tags then
        local tag_list = {}
        for tag in pairs(seg.tags) do
            table.insert(tag_list, tag)
        end

        if #tag_list > 0 then
            logger.debug("    " .. table.concat(tag_list, ", "))
        else
            logger.debug("    (无tags)")
        end
    else
        logger.debug("    tags is nil")
    end

    logger.debug("  selected_index: " .. tostring(seg.selected_index))

    local prompt = seg.prompt or ""
    if prompt ~= "" then
        logger.debug("  prompt: '" .. prompt .. "'")
    else
        logger.debug("  prompt: (空)")
    end

    -- 获取菜单信息
    if seg.menu then
        logger.debug("  menu:")
        logger.debug("    candidate_count: " .. tostring(seg.menu:candidate_count()))
        logger.debug("    empty: " .. tostring(seg.menu:empty()))

        -- 打印前几个候选项
        local count = seg.menu:candidate_count()
        if count > 0 then
            logger.debug("    前几个候选项:")
            for i = 0, math.min(count - 1, 4) do -- 最多显示5个
                local cand = seg.menu:get_candidate_at(i)
                if cand then
                    logger.debug(string.format("      %d. %s (%s)", i + 1, cand.text, cand.comment or ""))
                end
            end
        end
    else
        logger.debug("  menu: nil")
    end
end

-- 打印Translation的详细信息（用于一般调试，会消耗translation）
-- 注意：这个函数会消耗translation，如果在filter中使用需要返回候选词列表
function debug_utils.print_translation_info(translation, logger)
    if not translation then
        logger.debug("Translation is nil")
        return {}
    end

    logger.debug("=== Translation 对象信息 ===")
    logger.debug("exhausted: " .. tostring(translation.exhausted))

    -- 统计候选项数量
    local count = 0
    local all_candidates = {}

    -- 收集候选项信息
    for cand in translation:iter() do
        count = count + 1
        table.insert(all_candidates, cand)

        -- 限制日志记录数量避免日志过长
        if count > 10 then
            -- 继续收集但不记录到日志
        end
    end

    logger.debug("候选项数量: " .. count .. (count > 10 and " (显示前10个详细信息)" or ""))

    -- 打印前10个候选项详细信息
    if #all_candidates > 0 then
        logger.debug("")
        logger.debug("=== 候选项详细信息 ===")
        for i = 1, math.min(#all_candidates, 10) do
            debug_utils.print_candidate_info(all_candidates[i], i, logger)
            logger.debug("")
        end
    end

    -- 返回所有候选词
    return all_candidates
end

-- 打印Environment信息
function debug_utils.print_env_info(env, logger)
    if not env then
        logger.debug("Environment is nil")
        return
    end

    logger.debug("=== Environment 信息 ===")

    -- Engine信息
    if env.engine then
        logger.debug("Engine:")
        logger.debug("  schema_id: " .. (env.engine.schema and env.engine.schema.schema_id or "nil"))
        logger.debug("  active_engine: " .. tostring(env.engine.active_engine))

        -- Context信息
        if env.engine.context then
            local ctx = env.engine.context
            logger.debug("  Context:")
            logger.debug("    input: '" .. (ctx.input or "") .. "'")
            logger.debug("    caret_pos: " .. tostring(ctx.caret_pos))
            logger.debug("    commit_history: " .. tostring(ctx.commit_history))

            -- 获取选项状态
            local options = {"ascii_mode", "ascii_punct", "full_shape", "simplification"}
            logger.debug("    options:")
            for _, opt in ipairs(options) do
                local status = ctx:get_option(opt)
                logger.debug("      " .. opt .. ": " .. tostring(status))
            end

            -- Composition信息
            if ctx.composition then
                local comp = ctx.composition
                logger.debug("    Composition:")
                logger.debug("      empty: " .. tostring(comp:empty()))
                if not comp:empty() then
                    logger.debug("      length: " .. tostring(comp.length))

                    -- 获取最后一个segment
                    local back_seg = comp:back()
                    if back_seg then
                        logger.debug("      back segment info:")
                        debug_utils.print_segment_info(back_seg, logger)
                    end
                end
            end
        end
    end

    -- 检查名字空间中的变量
    logger.debug("Name space variables:")
    if env.name_space then
        logger.debug("env.name_space 存在，类型: " .. type(env.name_space))

        -- 添加错误捕获
        local success, error_msg = pcall(function()
            if type(env.name_space) == "table" then
                logger.debug("遍历打印 name_space 表中的内容:")
                local count = 0
                for k, v in pairs(env.name_space) do
                    logger.debug("  " .. tostring(k) .. ": " .. tostring(type(v)))
                    count = count + 1
                    -- 限制输出数量，避免日志过长
                    if count >= 20 then
                        logger.debug("  ... (显示前20项)")
                        break
                    end
                end
                if count == 0 then
                    logger.debug("  name_space 表为空")
                end
            elseif type(env.name_space) == "string" then
                logger.debug("name_space 是字符串: '" .. env.name_space .. "'")
            else
                logger.debug("name_space 是 " .. type(env.name_space) .. " 类型，值: " .. tostring(env.name_space))
            end
        end)

        if not success then
            logger.debug("处理 name_space 时发生错误: " .. tostring(error_msg))
        end
    else
        logger.debug("  name_space is nil")
    end
end

-- 打印Segmentation的详细信息
function debug_utils.print_segmentation_info(segmentation, logger)
    if not segmentation then
        logger.debug("Segmentation is nil")
        return
    end

    logger.debug("=== Segmentation 对象信息 ===")
    logger.debug("size: " .. tostring(segmentation.size))
    logger.debug("empty: " .. tostring(segmentation:empty()))
    logger.debug("segmentation.input: " .. segmentation.input)

    -- 获取已确认位置
    local confirmed_pos = segmentation:get_confirmed_position()
    logger.debug("confirmed_position: " .. tostring(confirmed_pos))
    local confirmed_pos_input = segmentation.input:sub(confirmed_pos + 1)
    logger.debug("confirmed_pos_input: " .. confirmed_pos_input)

    -- 获取当前位置信息
    local current_start = segmentation:get_current_start_position()
    local current_end = segmentation:get_current_end_position()
    local current_length = segmentation:get_current_segment_length()
    logger.debug("current_start_position: " .. tostring(current_start))
    logger.debug("current_end_position: " .. tostring(current_end))
    logger.debug("current_segment_length: " .. tostring(current_length))

    -- 检查是否完成分词
    logger.debug("has_finished_segmentation: " .. tostring(segmentation:has_finished_segmentation()))

    -- 打印所有Segment信息
    logger.debug("")
    logger.debug("=== 所有 Segment 信息 ===")
    local segments = segmentation:get_segments()

    if segments and #segments > 0 then
        for i, seg in ipairs(segments) do
            logger.debug("")
            logger.debug(string.format("Segment %d:", i))
            debug_utils.print_segment_info(seg, logger)
        end
    else
        logger.debug("没有segments")
    end

end

-- 打印Translation的详细信息（专门用于filter调试）
-- 注意：这个函数会消耗translation，需要返回收集到的候选词列表
function debug_utils.print_translation_detailed(translation, logger)
    if not translation then
        logger.debug("Translation is nil")
        return {}
    end

    logger.debug("=== Translation 详细调试信息 ===")
    logger.debug("exhausted: " .. tostring(translation.exhausted))

    -- 统计候选项数量和详细信息
    local count = 0
    local all_candidates = {} -- 存储所有候选词，用于重新yield

    -- 收集所有候选项信息
    for cand in translation:iter() do
        count = count + 1
        table.insert(all_candidates, cand) -- 保存原始候选词对象
    end

    logger.debug("候选项总数: " .. count)

    -- 打印前15个候选项的详细信息
    if #all_candidates > 0 then
        logger.debug()
        logger.debug("=== 候选项详细列表（前15个）===")
        for i = 1, math.min(#all_candidates, 5) do
            local cand = all_candidates[i]
            logger.debug(string.format("候选项 %d:", i))  -- 你好
            logger.debug("  type: " .. tostring(cand.type or "nil"))
            logger.debug("  start: " .. tostring(cand.start or "nil"))
            logger.debug("  _end: " .. tostring(cand._end or "nil"))
            logger.debug("  text: '" .. (cand.text or "") .. "'")
            logger.debug("  comment: '" .. (cand.comment or "") .. "'")
            logger.debug("  preedit: '" .. (cand.preedit or "") .. "'")
            logger.debug("  quality: " .. tostring(cand.quality or "nil"))
            logger.debug()
        end
    else
        logger.debug("没有候选项")
    end

    -- 返回所有候选词，让调用者重新yield
    return all_candidates

end

-- 打印Context的详细信息
function debug_utils.print_context_info(context, logger)
    if not context then
        logger.debug("Context is nil")
        return
    end

    logger.debug("=== Context 详细信息 ===")
    
    -- 基础属性
    logger.debug("基础属性:")
    logger.debug("  input: '" .. (context.input or "") .. "'")
    logger.debug("  caret_pos: " .. tostring(context.caret_pos))
    logger.debug("  is_composing: " .. tostring(context:is_composing()))
    logger.debug("  has_menu: " .. tostring(context:has_menu()))
    
    -- 获取文本相关信息
    logger.debug("文本信息:")
    local commit_text = context:get_commit_text()
    local script_text = context:get_script_text()
    logger.debug("  get_commit_text: '" .. (commit_text or "") .. "'")
    logger.debug("  get_script_text: '" .. (script_text or "") .. "'")
    
    -- 预编辑信息
    local preedit = context:get_preedit()
    if preedit then
        logger.debug("  preedit:")
        logger.debug("    text: '" .. (preedit.text or "") .. "'")
    else
        logger.debug("  preedit: nil")
    end
    
    -- 选中的候选词
    local selected_candidate = context:get_selected_candidate()
    if selected_candidate then
        logger.debug("  selected_candidate:")
        logger.debug("    text: '" .. (selected_candidate.text or "") .. "'")
        logger.debug("    comment: '" .. (selected_candidate.comment or "") .. "'")
        logger.debug("    type: " .. tostring(selected_candidate.type))
        logger.debug("    quality: " .. tostring(selected_candidate.quality))
    else
        logger.debug("  selected_candidate: nil")
    end

end

return debug_utils
