-- lua/rawenglish_translator.lua
-- 使用脚本翻译器处理反引号切分输入的translator
-- 添加了缓存功能, 加快速度
-- 通过text_splitter.split_by_rawenglish函数切分输入，对abc类型片段使用script_translator翻译
local text_splitter = require("text_splitter")
local logger_module = require("logger")
local debug_utils = require("debug_utils")
-- 引入spans管理模块
local spans_manager = require("spans_manager")

-- 创建当前模块的日志记录器
local logger = logger_module.create("rawenglish_translator_cache", {
    enabled = true, -- 启用日志以便测试
    unique_file_log = false, -- 启用日志以便测试
    log_level = "INFO"
})
-- 清空日志文件
logger.clear()

local rawenglish_translator = {}

-- 缓存：按片段索引保存上一次已计算的候选词（非最后一段）
local combo_cache = {} -- key: segment.original (fallback: segment.content), value: candidates list

-- 实时读取配置的函数
function rawenglish_translator.update_current_config(config)
    rawenglish_translator.rawenglish_delimiter_before = config:get_string("translator/rawenglish_delimiter_before")
    rawenglish_translator.rawenglish_delimiter_after = config:get_string("translator/rawenglish_delimiter_after")
    rawenglish_translator.delimiter = config:get_string("speller/delimiter"):sub(1, 1) or " "

    rawenglish_translator.replace_punct_enabled = config:get_bool("translator/replace_punct_enabled") or false
    rawenglish_translator.single_fuzhu = config:get_bool("aux_code/single_fuzhu") or false
    rawenglish_translator.fuzhu_mode = config:get_string("aux_code/fuzhu_mode") or ""
    rawenglish_translator.english_mode_symbol = config:get_string("translator/english_mode_symbol") or "`"
    combo_cache = {}  -- 更新配置时顺便清空缓存
end

-- 只在选词完成后回调, 判断当前输入内容当中, 第一个字符是否反引号,
-- 如果是反引号片段,则再次发送一次确认，确认当前第一个候选项.
-- local function rawenglish_before(context)
--     -- logger.debug("选择通知器: 处理反引号片段自动上屏开始")
--     -- 防止递归
--     if context:get_option("_bt_auto") then
--         return
--     end
--     context:set_option("_bt_auto", true)

--     -- 连续确认后续带有 rawenglish 标记的片段
--     logger.debug("选择通知器: 开始处理反引号片段自动上屏")
--     logger.debug("context:is_composing(): " .. tostring(context:is_composing()))
--     if context:is_composing() then
--         local composition = context.composition
--         local segment = composition:back() -- 当前光标所在段
--         -- debug_utils.print_segment_info(segment, logger)

--         local input = context.input
--         -- 对input进行切片
--         local segment_input = input:sub(segment.start + 1, segment._end) -- 获取当前段的输入内容
--         logger.debug("当前段的输入内容: " .. segment_input)

--         -- 检查当前seg对应的input, 是否以反引号开头,如果是以反引号开头,则提取出反引号包裹的范围, 直接确认选择.
--         if segment and segment_input:match("^`") then
--             logger.debug("当前段是反引号片段，选择第一个候选项")
--             -- context:commit()
--             if context:confirm_current_selection() then
--                 logger.debug("确认当前选择成功")
--             else
--                 logger.error("确认当前选择失败")
--             end
--         end
--     end

--     context:set_option("_bt_auto", false)
-- end

function rawenglish_translator.init(env)
    logger.debug("脚本反引号翻译器初始化开始")

    local engine = env.engine
    local config = env.engine.schema.config
    rawenglish_translator.update_current_config(config)
    logger.debug("rawenglish_delimiter_before: " .. rawenglish_translator.rawenglish_delimiter_before ..
                    " rawenglish_delimiter_after: " .. rawenglish_translator.rawenglish_delimiter_after)

    -- 创建script_translator组件
    env.script_translator = Component.Translator(engine, "translator", "script_translator")
    env.user_dict_set_translator = Component.Translator(engine, "user_dict_set", "script_translator")

    if env.script_translator then
        logger.debug("成功创建script_translator组件")
    else
        logger.error("创建script_translator组件失败")
    end

    if env.user_dict_set_translator then
        logger.debug("成功创建user_dict_set_translator组件")
    else
        logger.error("创建user_dict_set_translator组件失败")
    end

    logger.debug("脚本反引号翻译器初始化完成")

    -- 监听选词事件
    -- engine.context.select_notifier:connect(rawenglish_before)

end

-- 使用两个translator获取多个候选词，返回完整的Candidate列表
local function get_candidates(input, seg, env, max_count, allow_fallback)
    if not env.script_translator or not env.user_dict_set_translator then
        logger.error("script_translator 或 user_dict_set_translator未初始化")
        return {}
    end

    logger.debug()
    logger.debug("开始使用get_candidates获取候选词, 最大候选词数max_count: " .. max_count ..
                    ", 允许长度不足补全: " .. tostring(allow_fallback))
    logger.debug("查询两个translator，输入: " .. input .. ", 最大候选词数: " .. max_count)

    local valid_candidates = {} -- 存储长度匹配的候选词
    local fallback_candidates = {} -- 存储长度最长的候选词作为备选
    local segment_length = #input -- segment的长度就是input的长度

    -- 先尝试从 script_translator 获取候选词
    logger.debug("尝试从 script_translator 获取候选词...")
    local success1, translation1 = pcall(function()
        return env.script_translator:query(input, seg)
    end)

    if success1 and translation1 then
        local count1 = 0
        for cand in translation1:iter() do
            count1 = count1 + 1
            local cand_length = cand._end - cand.start
            logger.debug(string.format("script_translator候选词 %d: '%s', 长度: %d, segment长度: %d", count1,
                cand.text, cand_length, segment_length))

            if cand_length == segment_length then
                table.insert(valid_candidates, cand)
                logger.debug(string.format("script_translator候选词长度匹配，添加: '%s'", cand.text))

                -- 如果已经找到足够的候选词，直接返回
                if #valid_candidates >= max_count then
                    logger.debug("从script_translator已获取足够数量的候选词，直接返回")
                    return valid_candidates, false -- 返回是否使用了fallback
                end
            else
                -- 只有允许fallback时才收集备选候选词
                if allow_fallback then
                    table.insert(fallback_candidates, {
                        cand = cand,
                        length = cand_length
                    })
                    logger.debug(string.format("script_translator候选词长度不匹配，添加到备选列表: '%s'",
                        cand.text))
                else
                    logger.debug(string.format(
                        "script_translator候选词长度不匹配，不允许fallback，跳过: '%s'", cand.text))
                end
            end

            -- 限制遍历数量，只遍历前max_count个
            if count1 >= max_count then
                logger.debug("script_translator已遍历" .. max_count .. "个候选词，停止遍历")
                break
            end
        end
    else
        logger.error("调用script_translator失败: " .. tostring(translation1))
    end

    -- 如果第一个translator的候选词不足，尝试从 user_dict_set_translator 获取
    if #valid_candidates < max_count and env.user_dict_set_translator then
        logger.debug("script_translator候选词不足，尝试从 user_dict_set_translator 获取...")
        local success2, translation2 = pcall(function()
            return env.user_dict_set_translator:query(input, seg)
        end)

        if success2 and translation2 then
            local count2 = 0
            for cand in translation2:iter() do
                count2 = count2 + 1
                local cand_length = cand._end - cand.start
                logger.debug(string.format("user_dict_set_translator候选词 %d: '%s', 长度: %d, segment长度: %d",
                    count2, cand.text, cand_length, segment_length))

                if cand_length == segment_length then
                    table.insert(valid_candidates, cand)
                    logger.debug(string.format("user_dict_set_translator候选词长度匹配，添加: '%s'", cand.text))

                    -- 如果已经找到足够的候选词，停止
                    if #valid_candidates >= max_count then
                        logger.debug("已获取足够数量的候选词，停止获取")
                        break
                    end
                else
                    -- 只有允许fallback时才收集备选候选词
                    if allow_fallback then
                        table.insert(fallback_candidates, {
                            cand = cand,
                            length = cand_length
                        })
                        logger.debug(string.format(
                            "user_dict_set_translator候选词长度不匹配，添加到备选列表: '%s'", cand.text))
                    else
                        logger.debug(string.format(
                            "user_dict_set_translator候选词长度不匹配，不允许fallback，跳过: '%s'",
                            cand.text))
                    end
                end

                -- 限制遍历数量，只遍历前max_count个
                if count2 >= max_count then
                    logger.debug("user_dict_set_translator已遍历" .. max_count .. "个候选词，停止遍历")
                    break
                end
            end
        else
            logger.error("调用user_dict_set_translator失败: " .. tostring(translation2))
        end
    elseif not env.user_dict_set_translator then
        logger.debug("user_dict_set_translator未初始化")
    end

    local used_fallback = false

    -- 如果没有长度匹配的候选词，且允许fallback，使用长度最长的备选方案
    if #valid_candidates == 0 and #fallback_candidates > 0 and allow_fallback then
        logger.debug("没有长度匹配的候选词，使用长度最长的备选方案")
        used_fallback = true

        -- 按长度降序排序
        table.sort(fallback_candidates, function(a, b)
            return a.length > b.length
        end)

        -- 选择长度最长的max_count个候选词
        for i = 1, math.min(#fallback_candidates, max_count) do
            table.insert(valid_candidates, fallback_candidates[i].cand)
            logger.debug(string.format("使用备选候选词 %d: '%s', 长度: %d", i, fallback_candidates[i].cand.text,
                fallback_candidates[i].length))
        end
    end

    if #valid_candidates == 0 then
        logger.debug("未获取到任何候选词，返回空列表")
    else
        logger.debug("共获取到 " .. #valid_candidates .. " 个候选词" ..
                        (used_fallback and " (使用了fallback)" or ""))
        for i, cand in ipairs(valid_candidates) do
            logger.debug(string.format("最终候选词 %d: '%s'", i, cand.text))
        end
    end
    logger.debug()
    return valid_candidates, used_fallback
end

function rawenglish_translator.func(input, seg, env)
    local engine = env.engine
    local context = engine.context
    local context_input = context.input
    local config = engine.schema.config

    logger.debug("rawenglish_delimiter_before: " .. rawenglish_translator.rawenglish_delimiter_before ..
                    " rawenglish_delimiter_after: " .. rawenglish_translator.rawenglish_delimiter_after)

    -- 自动检查并清除过期的spans信息
    spans_manager.auto_clear_check(context, context_input)

    logger.debug("")
    logger.debug("开始处理输入: " .. input)
    logger.debug("seg信息: ")
    debug_utils.print_segment_info(seg, logger)
    local composition = env.engine.context.composition
    local segmentation = composition:toSegmentation()
    logger.debug("segmentation信息: ")
    debug_utils.print_segmentation_info(segmentation, logger)

    -- 检查输入如果长度是1, 而且只有一个英文模式符号, 则单独进行处理, 如果不是英文模式符号而且只有一个字符,则直接退出
    if #input == 1 then
        if input == "`" then
            local markdown_code_symbol = "```\n\n```"
            local candidate1 = Candidate("punct", seg.start, seg._end, markdown_code_symbol, "")
            candidate1.preedit = "`" 
            -- logger.debug("英文模式符号上屏: " .. input)
            local candidate2 = Candidate("punct", seg.start, seg._end, "`", "")
            yield(candidate1)
            yield(candidate2)
            return
        else

            return
        end

    end

    -- 检查输入是否包含反引号标签
    if not seg:has_tag("rawenglish_combo") and not seg:has_tag("single_rawenglish") then
        logger.debug("没有包含rawenglish或single_rawenglish标签，不处理")
        return
    end
    logger.debug("含有rawenglish_combo 或 single_rawenglish 标签, 进入反引号translator")

    -- 处理single_rawenglish类型的片段
    if seg:has_tag("single_rawenglish") then
        logger.debug("检测到single_rawenglish标签，进行特殊处理")

        -- 检查输入是否以反引号开头和结尾
        local inner_content, replaced_content

        if input:sub(-1) == rawenglish_translator.english_mode_symbol then
            -- 完整的反引号片段，提取反引号内的内容
            inner_content = input:sub(2, -2)
            logger.debug("完整反引号片段，内容: '" .. inner_content .. "'")
        else
            -- 未闭合的反引号片段，提取反引号后的内容
            inner_content = input:sub(2)
            logger.debug("未闭合反引号片段，内容: '" .. inner_content .. "'")
        end

        -- 替换成配置的分隔符
        -- 如果前后两个分隔符都是空格，则只添加后置分隔符，避免前置多余空格
        if rawenglish_translator.rawenglish_delimiter_before == " " and rawenglish_translator.rawenglish_delimiter_after ==
            " " then
            replaced_content = inner_content .. rawenglish_translator.rawenglish_delimiter_after
        else
            replaced_content = rawenglish_translator.rawenglish_delimiter_before .. inner_content ..
                                   rawenglish_translator.rawenglish_delimiter_after
        end
        logger.debug("替换后内容: '" .. replaced_content .. "'")

        -- 生成候选词
        local candidate = Candidate("single_rawenglish", seg.start, seg._end, replaced_content, "")
        candidate.preedit = input
        yield(candidate)

        logger.debug("已生成single_rawenglish候选词: '" .. replaced_content .. "'")
        return
    end

    -- 使用text_splitter.split_by_rawenglish切分输入
    -- 这里输入的input应该不是完整的input,而是剩余的seg当中的input,所以返回的也是这个结果,但是我需要确认前边已经有多少内容被确认了. 
    -- 这里是将当前片段的input输入进去, 但是前边可能有其他片段的input,导致计算出来的切分坐标不对.
    -- 计算在input前边还有多少已经处理完的内容, 在script计算的start和end值中添加这个长度
    local segments = text_splitter.split_by_rawenglish_with_log(input, seg.start, seg._end,
        rawenglish_translator.rawenglish_delimiter_before, rawenglish_translator.rawenglish_delimiter_after, logger)

    if not segments or #segments == 0 then
        logger.error("切分失败或无结果")
        return
    end

    -- 检查第一个片段是否为rawenglish类型，若是则直接commit_text并返回
    if segments[1].type == "rawenglish_combo" then
        -- 还要考虑新的可能性: 如果是只有一个反引号开头，如何判断
        -- 如果是单引号开头，然后后面跟着一些字母，或者其他内容，反引号暂时未闭合，如何处理？
        -- 如果是一个完整的反引号包裹的内容，如何处理？
        -- 判断,如果segments中只有一个元素,并且是rawenglish类型,

        -- 获取segment的基本信息
        local start_pos = seg.start -- 片段开始位置
        local end_pos = seg._end -- 片段结束位置  
        local length = seg.length -- 片段长度
        local status = seg.status -- 片段状态
        -- 打印信息
        logger.debug(string.format("片段信息: start=%d, end=%d, length=%d, status=%s", start_pos, end_pos, length,
            status))
        -- 打印开始和结束位置
        logger.debug(string.format("片段开始位置: %d, 结束位置: %d", segments[1].start, segments[1]._end))
        -- 在这里也应该添加chinese_pos数据, 后面标点符号替换的时候才能豁免.
        -- chinese_pos里面添加的是什么来着？应该是中文的部分,也就是说,如果第一段就是英文的话,应该只有一个候选词.
        local chinese_pos = "chinese_pos:" .. seg.start + segments[1].length .. "," .. seg.start + segments[1].length ..
                                ","
        -- logger.debug("chinese_pos: " .. chinese_pos)
        local cand_temp = Candidate("rawenglish_combo", seg.start, seg.start + segments[1].length, segments[1].content,
            chinese_pos)
        yield(cand_temp)

        return
    end

    -- 处理每个片段（仅重新计算最后一段），收集每个片段的候选词
    local segment_candidates = {} -- 存储每个片段的候选词列表
    local used_fallback = false -- 记录是否使用了fallback（仅可能发生在最后一段）
    local fallback_length_diff = 0 -- 记录fallback导致的长度差异（仅最后一段）
    local delete_last_code = false -- 紧挨着反引号的一个单独字母情况下（仅最后一段）
    local script_fail_code = 0 -- 反引号后面没有匹配成功的几位字母（仅最后一段）

    local seg_count = #segments
    -- 采用内容键缓存（segment.original）；无需按数量修剪

    -- 1) 先填充非最后一段：按内容键缓存；若缓存不存在（首次进入），计算一次并写入缓存
    for i = 1, math.max(0, seg_count - 1) do
        local segment = segments[i]
        local key = segment.original

        if combo_cache[key] then
            segment_candidates[i] = combo_cache[key]
            logger.info(string.format("片段 %d 复用缓存 key='%s'，共 %d 个候选项", i, key,
                #segment_candidates[i]))
        else
            local candidates_for_segment = {}

            if segment.type == "abc" then
                logger.info(string.format("[首轮缓存构建] 处理文本片段 %d: '%s' (key='%s')", i,
                    segment.content, key))
                local allow_fallback = false -- 非最后一段不允许fallback

                local segment_content = segment.content
                if segment_content ~= "" then
                    local candidates, segment_used_fallback =
                        get_candidates(segment_content, seg, env, 2, allow_fallback)
                    logger.info("get_candidates(非末段) 返回 segment_used_fallback: " ..
                                    tostring(segment_used_fallback))

                    if #candidates > 0 then
                        for index, cand in ipairs(candidates) do
                            table.insert(candidates_for_segment, {
                                text = cand.text,
                                preedit = cand.preedit or segment.content,
                                spans = cand:spans(),
                                start = segment.start,
                                _end = segment._end,
                                length = segment.length,
                                type = segment.type
                            })
                        end
                    else
                        -- 非末段没有候选则保留原样
                        local other_spans = Spans()
                        other_spans:add_span(segment.start, segment._end)
                        table.insert(candidates_for_segment, {
                            text = segment.content,
                            preedit = segment.content,
                            spans = other_spans,
                            start = segment.start,
                            _end = segment._end,
                            length = segment.length,
                            type = segment.type
                        })
                    end
                end

            elseif segment.type == "rawenglish_combo" then
                logger.info(string.format("[首轮缓存构建] 处理反引号片段 %d: '%s' (key='%s')", i,
                    segment.content, key))
                local rawenglish_spans = Spans()
                rawenglish_spans:add_span(segment.start, segment._end)
                table.insert(candidates_for_segment, {
                    text = segment.content,
                    preedit = segment.original or segment.content,
                    spans = rawenglish_spans,
                    start = segment.start,
                    _end = segment._end,
                    length = segment.length,
                    type = segment.type
                })

            else
                logger.info(string.format(
                    "[首轮缓存构建] 处理其他类型片段 %d: type=%s, content='%s' (key='%s')", i,
                    segment.type, segment.content, key))
                local other_spans = Spans()
                other_spans:add_span(segment.start, segment._end)
                table.insert(candidates_for_segment, {
                    text = segment.content,
                    preedit = segment.content,
                    spans = other_spans,
                    start = segment.start,
                    _end = segment._end,
                    length = segment.length,
                    type = segment.type
                })
            end

            segment_candidates[i] = candidates_for_segment
            combo_cache[key] = candidates_for_segment -- 写入按内容键的缓存
            logger.info(string.format("片段 %d 首次构建并缓存 %d 个候选项, key='%s'", i,
                #candidates_for_segment, key))
        end
    end

    -- 2) 计算最后一段：每次都重新计算
    if seg_count >= 1 then
        local i = seg_count
        local segment = segments[i]
        local candidates_for_segment = {}

        if segment.type == "abc" then
            logger.info(string.format("处理最后一个文本片段 %d: '%s'", i, segment.content))

            local is_last_segment = true
            local allow_fallback = true -- 只有最后一个允许fallback

            local segment_content = segment.content
            -- 等于说在这个脚本里面对辅助码进行了单独的处理, 看看能不能放到那个脚本当中, 而不是放到这里
            if rawenglish_translator.single_fuzhu and rawenglish_translator.fuzhu_mode == "all" then
                
                if is_last_segment then
                    -- 对于英文模式之后的第一个字母, 应该豁免, 这里只是标记delete_last_code = true, segment_content中切除一个字母, 和这里应该没关系
                    local has_punctuation = segment_content:match("[,.!?;:()%[%]<>/_=+*&^%%$#@~|%-'\"']") ~= nil
                    if has_punctuation then
                        logger.info("有标点符号")
                        local segment_content_nopunc = segment_content:gsub("[,.!?;:()%[%]<>/_=+*&^%%$#@~|%-'\"']", "")
                        logger.info("删除标点符号后的segment_content: " .. segment_content_nopunc)
                        if #segment_content_nopunc % 2 == 1 and #segment_content_nopunc ~= 1 then
                            segment_content = segment_content:sub(1, -2)
                            delete_last_code = true
                            logger.info("调整后segment_content: " .. segment_content)
                        end
                    else
                        if #segment_content % 2 == 1 and #segment_content ~= 1 then
                            segment_content = segment_content:sub(1, -2)
                            delete_last_code = true
                            logger.info("调整后segment_content: " .. segment_content)
                        end
                    end
                end
            end

            if segment_content ~= "" then
                local candidates, segment_used_fallback = get_candidates(segment_content, seg, env, 2, allow_fallback)
                logger.info("get_candidates(末段) 返回 segment_used_fallback: " .. tostring(segment_used_fallback))

                if #candidates == 0 then
                    script_fail_code = segment.length
                end

                if segment_used_fallback then
                    used_fallback = true
                    if #candidates > 0 then
                        local cand = candidates[1]
                        local cand_length = cand._end - cand.start
                        fallback_length_diff = #segment.content - cand_length
                        logger.info(string.format("使用fallback，fallback_length_diff差异: %d",
                            fallback_length_diff))
                    end
                end

                for index, cand in ipairs(candidates) do
                    table.insert(candidates_for_segment, {
                        text = cand.text,
                        preedit = cand.preedit or segment.content,
                        spans = cand:spans(),
                        start = segment.start,
                        _end = segment._end,
                        length = segment.length,
                        type = segment.type
                    })
                end
            end

        elseif segment.type == "rawenglish_combo" then
            logger.info(string.format("处理最后一个反引号片段 %d: '%s'", i, segment.content))
            local rawenglish_spans = Spans()
            rawenglish_spans:add_span(segment.start, segment._end)
            table.insert(candidates_for_segment, {
                text = segment.content,
                preedit = segment.original or segment.content,
                spans = rawenglish_spans,
                start = segment.start,
                _end = segment._end,
                length = segment.length,
                type = segment.type
            })

        else
            logger.info(string.format("处理最后一个其他类型片段 %d: type=%s, content='%s'", i, segment.type,
                segment.content))
            local other_spans = Spans()
            other_spans:add_span(segment.start, segment._end)
            table.insert(candidates_for_segment, {
                text = segment.content,
                preedit = segment.content,
                spans = other_spans,
                start = segment.start,
                _end = segment._end,
                length = segment.length,
                type = segment.type
            })
        end

        segment_candidates[i] = candidates_for_segment
        do
            local key = segment.original or segment.content or ("#" .. tostring(i))
            combo_cache[key] = candidates_for_segment -- 也按内容键缓存，便于跨多次输入复用
        end
        logger.info(string.format("最后片段 %d 收集到 %d 个候选项", i, #candidates_for_segment))
    end

    -- 生成所有可能的组合
    local function generate_combinations(segment_lists, current_combination, current_index, all_combinations)
        if current_index > #segment_lists then
            -- 达到末尾，保存当前组合
            table.insert(all_combinations, current_combination)
            return
        end

        -- 遍历当前片段的所有候选词
        for _, candidate in ipairs(segment_lists[current_index]) do
            local new_combination = {}
            -- 复制当前组合
            for j = 1, #current_combination do
                new_combination[j] = current_combination[j]
            end
            -- 添加新的候选词
            table.insert(new_combination, candidate)

            -- 递归处理下一个片段
            generate_combinations(segment_lists, new_combination, current_index + 1, all_combinations)
        end
    end

    local all_combinations = {}
    generate_combinations(segment_candidates, {}, 1, all_combinations)

    logger.debug("共生成 " .. #all_combinations .. " 个组合")

    -- 输出每个组合作为候选词，最多输出4个
    local output_count = 0
    local max_output = 4

    for combo_index, combination in ipairs(all_combinations) do
        output_count = output_count + 1
        if output_count >= max_output then
            logger.debug("已达到最大输出数量限制 " .. max_output .. "，停止输出")
            break
        end

        local final_text = ""
        local final_preedit = ""

        local count = 0
        local long_span = nil
        local text_len = 0
        local chinese_pos = "chinese_pos:"
        for _, one_cand in ipairs(combination) do
            count = count + 1
            final_text = final_text .. one_cand.text
            final_preedit = final_preedit .. one_cand.preedit

            -- 计算反引号片段的索引位置
            if one_cand.type == "abc" then
                local pos_start = text_len + 1
                text_len = text_len + utf8.len(one_cand.text)
                local pos_end = text_len
                chinese_pos = chinese_pos .. pos_start .. "," .. pos_end .. ","
            else
                text_len = text_len + #one_cand.text
            end
            -- 字符串保存格式: abc_pos:6,8,12,18

            -- 计算segment_cand的spans:
            -- 如果是abc类型的: spans应该是从0开始计算的, 当前片段长度10, 0-10,中间有一些分割点
            -- 如果是第一个候选词的时候需要处理
            if output_count == 1 then
                local success, err = pcall(function()
                    if count == 1 then
                        -- count代表第一个候选词,提取原生的spans信息
                        long_span = one_cand.spans
                        -- 对于第二个seg开始的就单独处理,如果是拼音,
                    elseif count > 1 and one_cand.type == "abc" then
                        -- 全部累加, segment_cand.start 就是每一段的开始, 所以原来是0开始,改成从start开始

                        -- 对segment_cand.spans() 中的Spans进行遍历,然后对每个分割点依次进行累加.
                        -- 如果是后面的abc段 例如 nihk`okok`wo 则返回的应该是0,2, 0,2累加one_cand.start, 这个候选词是从哪个位置开始的
                        local last_span = one_cand.spans
                        local vertices = last_span.vertices

                        for i, vertex in ipairs(vertices) do
                            -- logger.debug("vertex: " .. i .. ": " .. vertex)
                            -- logger.debug("segment_cand.start: " .. tostring(one_cand.start))
                            long_span:add_vertex(one_cand.start + vertex)
                        end

                    elseif count > 1 and one_cand.type == "rawenglish_combo" then
                        -- 这里是现在重点出错的地方
                        logger.debug(string.format(
                            "one_cand属性: text='%s', preedit='%s', start=%d, _end=%d, length=%d, type=%s",
                            tostring(one_cand.text), tostring(one_cand.preedit), tostring(one_cand.start),
                            tostring(one_cand._end), tostring(one_cand.length), tostring(one_cand.type)))
                        if one_cand.spans then
                            local spans_vertices = one_cand.spans.vertices
                            for vi, vertex in ipairs(spans_vertices) do
                                logger.debug(string.format("one_cand.spans.vertices[%d]=%d", vi, vertex))
                            end
                        else
                            logger.debug("one_cand.spans=nil")
                        end

                        long_span:add_span(one_cand.start, one_cand._end)
                    else
                        long_span:add_span(one_cand.start, one_cand._end)
                    end

                end)

                if not success then
                    logger.error("处理段落spans时出错: " .. tostring(err))
                    error("处理段落spans时出错: " .. tostring(err))
                end
            end

        end

        if output_count == 1 then
            -- 将 vertices 转换为字符串格式保存到属性

            local vertices = long_span.vertices

            for i, vertex in ipairs(vertices) do
                logger.debug("long_span.vertices " .. i .. ": " .. vertex)
            end

            -- 使用spans_manager保存spans信息（最高优先级）
            spans_manager.save_spans(context, vertices, context_input, "rawenglish_translator")

            logger.debug("long_span 分割点信息已保存到spans_manager")

            -- 检查spans信息是否保存成功
            local existing_spans = spans_manager.get_spans(context)
            if not existing_spans then
                logger.error("spans_manager保存失败，spans信息可能丢失")
            end
        end

        logger.debug(string.format("组合 %d: text='%s', preedit='%s'", combo_index, final_text, final_preedit))

        -- 如果最终结果与原输入不同，则输出候选词
        if final_text ~= input and final_text ~= "" then
            -- 如果使用了fallback，需要调整候选词的结束位置
            local candidate_end = seg._end
            if delete_last_code then
                -- 如果删除了奇数个字母最后一个, 则seg向左移动一位
                candidate_end = candidate_end - 1
            end

            if script_fail_code > 0 then
                -- 最后一段abc如果没有匹配成功任何候选词的情况下, 向前移动
                candidate_end = candidate_end - script_fail_code
            end

            logger.debug("used_fallback的值: " .. tostring(used_fallback) .. "  fallback_length_diff的值: " ..
                            tostring(fallback_length_diff))
            if used_fallback and fallback_length_diff > 0 then
                logger.debug(string.format("使用了fallback，调整候选词结束位置: %d -> %d (差异: %d)",
                    seg._end, candidate_end, fallback_length_diff))
                candidate_end = seg._end - fallback_length_diff
            end

            -- local candidate = Candidate("sentence", seg.start, candidate_end, final_text,
            -- string.format("   [组合%d]", combo_index))
            -- 这是原来的候选词, 现在需要在第五个参数中传入反引号范围信息
            logger.debug("output_count为: " .. output_count .. " 候选词: " .. final_text ..
                             "  生成rawenglish_pos值为: " .. chinese_pos)
            -- 如果final_text中有标点符号,才需要添加chinese_pos
            local candidate = ""
            if text_splitter.has_punctuation_no_rawenglish(final_text, logger) then
                candidate = Candidate("rawenglish_combo", seg.start, candidate_end, final_text, chinese_pos)
            else
                candidate = Candidate("rawenglish_combo", seg.start, candidate_end, final_text, "")
            end

            -- local candidate = Candidate("rawenglish_combo", seg.start, candidate_end, final_text, chinese_pos)
            candidate.preedit = final_preedit
            yield(candidate)

            logger.debug(string.format("输出组合候选词 %d: text='%s', preedit='%s' (第%d个输出), end=%d",
                combo_index, final_text, final_preedit, output_count, candidate_end))
        end
    end

end

function rawenglish_translator.fini(env)
    logger.debug("脚本反引号翻译器结束运行")
end

return rawenglish_translator
