-- lua/baidu_filter.lua 修改成filter版本,通过百度云接口获取云输入法拼音词组,并添加到候选词中第一位中来
-- 百度云输入获取filter版本
-- - 20250718打算整个百度云输入获取和AI输入法的功能, 两个恐怕必须要放在一起，不太好拆开开发.
local json = require("json")

-- 引入日志工具模块
local logger_module = require("logger")
-- 引入文本切分模块
local text_splitter = require("text_splitter")
local debug_utils = require("debug_utils")
-- 引入spans管理模块
local spans_manager = require("spans_manager")

-- 创建当前模块的日志记录器
local logger = logger_module.create("cloud_ai_filter_v2", {
    enabled = true, -- 启用日志以便测试
    unique_file_log = false, -- 启用日志以便测试
    log_level = "DEBUG"
})
-- 清空日志文件
logger.clear()

-- 添加 ARM64 Homebrew 的 Lua 路径
local function setup_lua_paths()
    -- 保存原始路径
    local original_path = package.path
    local original_cpath = package.cpath

    -- 添加 ARM64 Homebrew 路径
    package.path = package.path .. ";/opt/homebrew/share/lua/5.4/?.lua;/opt/homebrew/share/lua/5.4/?/init.lua"
    package.cpath = package.cpath .. ";/opt/homebrew/lib/lua/5.4/?.so;/opt/homebrew/lib/lua/5.4/?/core.so"

    logger.info("已添加 ARM64 Homebrew Lua 路径")
end

setup_lua_paths()

local tcp_socket = nil
local ok, err = pcall(function()
    tcp_socket = require("tcp_socket_sync")
end)
if not ok then
    logger.error("加载 tcp_socket_sync 失败: " .. tostring(err))
else
    logger.info("加载 tcp_socket_sync 成功")
    if tcp_socket then
        logger.info("sync_module不为nil")
    else
        logger.error("sync_module为nil，尽管require没有报错")
    end
end

-- 云输入结果缓存机制
local cloud_result_cache = {
    last_input = "", -- 上次输入的内容
    cloud_candidates = {}, -- 缓存的云候选词
    ai_candidates = {}, -- 缓存的AI候选词
    timestamp = 0, -- 缓存时间戳
    cache_timeout = 60 -- 缓存有效期（秒）
}

-- 模块级配置缓存
local cloud_ai_filter = {}
cloud_ai_filter.behavior = {}
cloud_ai_filter.chat_triggers = {}
cloud_ai_filter.chat_names = {}
cloud_ai_filter.schema_name = ""
cloud_ai_filter.shuru_schema = ""
cloud_ai_filter.max_cloud_candidates = 2
cloud_ai_filter.max_ai_candidates = 1
cloud_ai_filter.delimiter = " "
cloud_ai_filter.rawenglish_delimiter_before = ""
cloud_ai_filter.rawenglish_delimiter_after = ""
cloud_ai_filter.ziranma_mapping_config = nil

-- 配置更新函数
function cloud_ai_filter.update_current_config(config)
    logger.info("开始更新cloud_ai_filter_v2模块配置")

    -- 读取 behavior 配置
    cloud_ai_filter.behavior = {}
    cloud_ai_filter.behavior.prompt_chat = config:get_string("ai_assistant/behavior/prompt_chat")

    -- 重新初始化配置表
    cloud_ai_filter.chat_triggers = {}
    cloud_ai_filter.chat_names = {}

    -- 获取 ai_prompts 配置项（新结构）
    local ai_prompts_config = config:get_map("ai_assistant/ai_prompts")
    if ai_prompts_config then
        local trigger_keys = ai_prompts_config:keys()
        logger.info("找到 " .. #trigger_keys .. " 个 ai_prompts 配置")

        -- 遍历 ai_prompts 中的所有触发器
        for _, trigger_name in ipairs(trigger_keys) do
            local base_key = "ai_assistant/ai_prompts/" .. trigger_name

            local trigger_value = config:get_string(base_key .. "/chat_triggers")
            local chat_name = config:get_string(base_key .. "/chat_names")

            if trigger_value and #trigger_value > 0 then
                cloud_ai_filter.chat_triggers[trigger_name] = trigger_value
                logger.info("AI触发器 - " .. trigger_name .. ": " .. trigger_value)
            end

            if chat_name and #chat_name > 0 then
                cloud_ai_filter.chat_names[trigger_name] = chat_name
                logger.info("AI聊天名称 - " .. trigger_name .. ": " .. chat_name)
            end
        end
    else
        logger.warn("未找到 ai_prompts 配置")
    end

    -- 读取其他配置项
    cloud_ai_filter.shuru_schema = config:get_string("schema/my_shuru_schema") or ""

    -- 读取候选词数量限制配置
    cloud_ai_filter.max_cloud_candidates = config:get_int("cloud_ai_filter/max_cloud_candidates") or 2
    cloud_ai_filter.max_ai_candidates = config:get_int("cloud_ai_filter/max_ai_candidates") or 1

    -- 读取分隔符配置
    cloud_ai_filter.delimiter = config:get_string("speller/delimiter"):sub(1, 1) or " "

    -- 读取反引号分隔符配置
    cloud_ai_filter.rawenglish_delimiter_before = config:get_string("cloud_ai_filter/rawenglish_delimiter_before") or ""
    cloud_ai_filter.rawenglish_delimiter_after = config:get_string("cloud_ai_filter/rawenglish_delimiter_after") or ""

    -- 加载自然码映射表
    cloud_ai_filter.ziranma_mapping_config = config:get_map("speller/ziranma_to_quanpin")

    logger.info("云候选词最大数量: " .. cloud_ai_filter.max_cloud_candidates)
    logger.info("AI候选词最大数量: " .. cloud_ai_filter.max_ai_candidates)
    logger.info("当前分隔符: " .. cloud_ai_filter.delimiter)

    logger.info("cloud_ai_filter_v2模块配置更新完成")
end

local replace_punct_enabled = false

-- 缓存管理函数
local function save_cloud_result_cache(input_text, parsed_data)
    if parsed_data and (parsed_data.cloud_candidates or parsed_data.ai_candidates) then
        cloud_result_cache.last_input = input_text
        cloud_result_cache.cloud_candidates = parsed_data.cloud_candidates or {}
        cloud_result_cache.ai_candidates = parsed_data.ai_candidates or {}
        cloud_result_cache.timestamp = os.time()
        logger.info("保存云输入结果缓存，输入: " .. input_text .. ", 云候选词: " ..
                        #cloud_result_cache.cloud_candidates .. ", AI候选词: " .. #cloud_result_cache.ai_candidates)
    end
end

local function get_cached_cloud_result(input_text)
    -- 检查缓存是否有效
    local current_time = os.time()
    if cloud_result_cache.last_input == input_text and cloud_result_cache.timestamp > 0 and
        (current_time - cloud_result_cache.timestamp) < cloud_result_cache.cache_timeout and
        (#cloud_result_cache.cloud_candidates > 0 or #cloud_result_cache.ai_candidates > 0) then

        logger.info("使用缓存的云输入结果，输入: " .. input_text .. ", 云候选词: " ..
                        #cloud_result_cache.cloud_candidates .. ", AI候选词: " .. #cloud_result_cache.ai_candidates)

        return {
            cloud_candidates = cloud_result_cache.cloud_candidates,
            ai_candidates = cloud_result_cache.ai_candidates
        }
    end

    return nil
end

local function clear_cloud_result_cache()
    cloud_result_cache.last_input = ""
    cloud_result_cache.cloud_candidates = {}
    cloud_result_cache.ai_candidates = {}
    cloud_result_cache.timestamp = 0
    logger.info("清空云输入结果缓存")
end

local function set_cloud_convert_flag(cand, context, delimiter)
    -- 这部分代码时检测输入的字符长度，通过检测中间有几个分隔符实现
    -- 检查当前是否正在组词状态（即用户正在输入但还未确认）
    local is_composing = context:is_composing()
    local preedit_text = cand.preedit
    -- 移除光标符号和后续的prompt内容
    local clean_text = preedit_text:gsub("‸.*$", "") -- 从光标符号开始删除到结尾
    logger.info("当前预编辑文本: " .. clean_text)
    local _, count = string.gsub(clean_text, delimiter, delimiter)
    logger.info("当前输入内容分隔符数量: " .. count)
    -- local has_punct = has_punctuation(input)

    -- 触发状态改成,当数如字符超过4个,或者有标点且超过2个:
    if is_composing and count >= 3 then
        logger.info("当前正在组词状态,检测到分隔符数量达到3,触发云输入提示")
        -- 只在值真正需要改变时才设置
        -- 先获取当前选项的值，避免不必要的更新
        logger.info("当前云输入提示标志: " .. context:get_property("cloud_convert_flag"))

        if context:get_property("cloud_convert_flag") == "0" then
            logger.info("云输入提示标志为 0, 设置为 1")
            context:set_property("cloud_convert_flag", "1")
            -- context:set_option("cloud_convert_prompt", true)
            logger.info("cloud_convert_flag 已设置为 1")

        end

    else
        -- 如果不在组词状态或没有达到触发条件,则重置提示选项
        logger.info("当前不在组词状态或未达到触发条件,云输入提示已重置")
        if context:get_property("cloud_convert_flag") == "1" then
            -- context:set_option("cloud_convert_prompt", false)
            context:set_property("cloud_convert_flag", "0")
            logger.info("cloud_convert_flag 已设置为 0")

        end
    end
end

function cloud_ai_filter.init(env)
    -- 初始化时清空日志文件
    logger.info("云输入处理器初始化完成")

    -- 获取 schema 信息，配置更新由 cloud_input_processor 统一管理
    local config = env.engine.schema.config
    cloud_ai_filter.schema_name = env.engine.schema.schema_name
    logger.info("等待 cloud_input_processor 统一更新配置")

    -- 清空云输入结果缓存
    clear_cloud_result_cache()

    logger.info("AI助手配置加载完成")
end

function cloud_ai_filter.func(translation, env)
    local engine = env.engine
    local context = engine.context
    local input = context.input

    -- 自动检查并清除过期的spans信息
    -- spans_manager.auto_clear_check(context, input)

    -- 检查输入是否包含标点符号或反引号
    -- local has_punctuation = confirmed_pos_input:match("[,.!?;:()%[%]<>/_=+*&^%%$#@~|%-`'\"']") ~= nil

    -- 包含标点符号或反引号，使用智能切分处理

    local segment = ""

    -- 在segment后面添加prompt
    local composition = context.composition
    local segmentation = composition:toSegmentation()
    local confirmed_pos_input = ""
    if (not segmentation:empty()) then
        -- 获得队尾的 Segment 对象
        segment = segmentation:back()
        -- local confirmed_pos = segmentation:get_confirmed_position()
        -- logger.info("segmentation:get_confirmed_position(): " .. confirmed_pos)
        -- confirmed_pos_input = input:sub(confirmed_pos + 1)

        -- logger.info("confirmed_pos_input: " .. confirmed_pos_input)

        -- -- 提取第一段segment,看看标签是不是 "ai_talk", 如果是这个标签,则将这个片段变成segment.status ~= "kConfirmed" 
        -- -- 那么需要调整segmente_input
        -- debug_utils.print_segmentation_info(segmentation, logger)
        -- local first_segment = segmentation:get_at(0)
        -- if first_segment:has_tag("ai_talk") then
        --     local ai_segment_length = first_segment._end - first_segment.start
        --     logger.info("发现AI段落，长度: " .. ai_segment_length .. "，内容: " ..
        --                     input:sub(first_segment.start + 1, first_segment._end))

        --     first_segment.status = "kConfirmed" 
        --     debug_utils.print_segmentation_info(segmentation, logger)
        -- end
    else
        logger.info("segmentation:empty 为空,直接返回: " .. tostring(segmentation:empty()))
        return
    end

    --  判断segment:has_tag("ai_prompt") , 给前x个候选词添加comment, x的数量和lua/ai_assistant_segmentor.lua中trigger_prefix:sub(1, 1) == prompt_chat 的数量相同, 
    -- 将每一个匹配上的prompt_triggers, 添加到候选词的comment当中去
    -- 所有ai_prompt就是当前的a字符，所以这里不用分析

    -- 检查是否是AI提示段落
    local is_ai_prompt = segment:has_tag("ai_prompt")
    if is_ai_prompt then
        logger.debug("检测到ai_prompt标签，开始处理AI提示候选词")

        -- 生成prompt_triggers列表，与ai_assistant_segmentor.lua中的逻辑一致
        local prompt_triggers = {}
        if cloud_ai_filter.behavior and cloud_ai_filter.chat_triggers then
            local prompt_chat = cloud_ai_filter.behavior.prompt_chat
            if prompt_chat then
                for trigger_name, trigger_prefix in pairs(cloud_ai_filter.chat_triggers) do
                    if trigger_prefix:sub(1, 1) == prompt_chat then
                        local chat_name = cloud_ai_filter.chat_names[trigger_name]
                        if chat_name then
                            local chat_name_clear = chat_name:gsub(":$", "")
                            table.insert(prompt_triggers, trigger_prefix .. chat_name_clear)
                        end
                    end
                end

                -- 排序以保持一致性
                table.sort(prompt_triggers)
                logger.info("生成了 " .. #prompt_triggers .. " 个提示触发器")
            end
        end

        -- 为候选词添加comment，每个候选词对应两个触发器
        local count = 0
        local max_rounds = math.floor(#prompt_triggers / 2) -- 计算最大轮数
        local current_round = 0

        for cand in translation:iter() do
            current_round = current_round + 1

            -- 如果超过最大轮数，不再添加comment
            if current_round <= max_rounds then
                count = count + 2
                local trigger_info1 = prompt_triggers[count - 1]
                local trigger_info2 = prompt_triggers[count]

                -- 组合触发器信息
                local combined_trigger_info = trigger_info1
                if trigger_info2 then
                    combined_trigger_info = combined_trigger_info .. "  " .. trigger_info2
                end

                cand.comment = " " .. combined_trigger_info
                logger.info("为候选词添加提示: " .. combined_trigger_info)
            end

            yield(cand)
        end

        logger.info("AI提示候选词处理完成，共处理 " .. count .. " 个候选词")
        return
    end

    local segments = {}

    local first_original_cand = nil
    local original_preedit = ""
    local cand_text
    local cand_start = 0
    local cand_end = 0
    local cand_type = nil
    local cand_comment = ""
    local spans = nil

    -- 首先检查是不是标点符号的候选词, 如果是直接确认第一个候选项,并返回.
    -- 先保存第一个原始候选词
    local long_candidates_table = {}
    local no_long_candidates_table = {}
    local count = 0
    for cand in translation:iter() do

        count = count + 1
        if count == 1 then
            first_original_cand = cand
            table.insert(long_candidates_table, cand)
            original_preedit = cand.preedit
            cand_text = cand.text
            cand_start = cand.start
            cand_end = cand._end
            cand_type = cand.type
            cand_comment = cand.comment
            -- logger.info(string.format(
            --     "原始候选词信息: text=%s, preedit=%s, start=%s, end=%s, type=%s, comment=%s",
            --     tostring(cand_text), tostring(original_preedit), tostring(cand_start), tostring(cand_end),
            --     tostring(cand_type), tostring(cand_comment)))
        end
        -- 只提取一个候选词
        break
    end

    -- 前边是只记录下来第一个候选词,然后这里提取第一个候选词的类型, 是标点符号, 或者是以"ai_chat"结尾的, 就输出第一个候选词, 
    -- 对剩余的候选词进行遍历, 输出候选词信息, 输出候选词, 然后返回
    if cand_type == "punct" or cand_type:sub(-7) == "ai_chat" then
        logger.debug("cand_type: punct or ai_chat cand_text: " .. cand_text)
        -- 输出原始候选词
        yield(first_original_cand)

        for cand in translation:iter() do
            logger.debug(string.format(
                "punct剩余选词信息: text=%s, preedit=%s, start=%s, end=%s, type=%s, comment=%s",
                tostring(cand.text), tostring(cand.preedit), tostring(cand.start), tostring(cand._end),
                tostring(cand.type), tostring(cand.comment)))
            yield(cand)
        end

        return
    else
        logger.debug("cand_type:  " .. cand_type)
    end

    -- 第一次进入的时候cloud_convert为true, 如果为false 则直接返回. 第二次如果这个有一个为真, 则
    if context:get_property("cloud_convert") ~= "1" and context:get_property("get_cloud_stream") ~= "starting" then
        logger.info("not cloud_convert, get_cloud_stream ~= starting")
        -- 查看有没有云翻译的标识, 没有的话直接返回原有的候选词
        yield(first_original_cand) -- 输出原有第一个候选词
        set_cloud_convert_flag(first_original_cand, context, cloud_ai_filter.delimiter)
        for cand in translation:iter() do
            yield(cand) -- 输出原有候选词
        end

        return

    end

    -- 代码走到这里,代表已经进入context:get_property("cloud_convert") == "1" 成立分支
    -- 首次触发云输入（发送请求并开始流式获取）
    logger.info("已经进入云输入法分支: cloud_convert " .. tostring(context:get_property("cloud_convert")) ..
                    " get_cloud_stream: " .. context:get_property("get_cloud_stream"))
    logger.info("cand_text: " .. cand_text .. " cand_type: " .. cand_type)

    if context:get_property("cloud_convert") == "1" then
        local ok, err = pcall(function()
            -- 长度足够的候选词放入到long_candidates_table, 不够的放到no_long_candidates_table,只放一个

            for cand in translation:iter() do
                if cand._end == segment._end then
                    table.insert(long_candidates_table, cand)
                else
                    table.insert(no_long_candidates_table, cand)
                    break
                end
            end

            local segment_input = input:sub(segment._start + 1, segment._end)
            logger.info("根据segment切片得到 segment_input: " .. segment_input)

            -- 发送翻译请求（异步，不等待响应）
            local send_success = tcp_socket.send_convert_request(cloud_ai_filter.schema_name,
                cloud_ai_filter.shuru_schema, segment_input, long_candidates_table)
            if send_success then
                logger.info("云输入翻译请求发送成功，开始流式获取结果")
                context:set_property("get_cloud_stream", "starting")
                env.first_read_convert_result = true
            else
                logger.error("云输入翻译请求发送失败")
                context:set_property("get_cloud_stream", "error")
                logger.info("get_cloud_stream, 设置为error")
                -- 在这里代表没有发送成功,则应该提示用户错误.
                -- segment.prompt = " [服务端未连接] "
                -- logger.warn("segment.prompt:  [服务端未连接] ")
            end
        end)
        if not ok then
            logger.error("tcp_socket.send_convert_request 调用失败: " .. tostring(err))
            context:set_property("get_cloud_stream", "error")
            logger.info("get_cloud_stream, 设置为error")
        end
    end

    -- 检查是否正在流式获取云输入结果
    if context:get_property("get_cloud_stream") == "starting" then
        logger.info("正在流式获取云输入结果，读取最新数据...")

        local ok, err = pcall(function()
            -- 读取云输入结果（流式读取）
            local timeout = 0.01
            if context:get_property("cloud_convert") == "1" then
                context:set_property("cloud_convert", "0") -- 重置选项，避免重复触发
            end
            local stream_result = tcp_socket.read_convert_result(timeout)
            local ordered_candidates = {}
            local segment_input = input:sub(segment._start + 1, segment._end)

            -- 云输入首次触发完成, 设置成假后续不再发送请求,只接收数据

            if stream_result and stream_result.status == "success" and stream_result.data then
                local parsed_data = stream_result.data
                logger.info("成功读取到云输入结果数据")

                -- 保存成功获取的数据到缓存
                save_cloud_result_cache(segment_input, parsed_data)

                -- 处理云输入结果数据，构建候选词
                if parsed_data.cloud_candidates then
                    for i, cloud_cand in ipairs(parsed_data.cloud_candidates) do
                        if i <= cloud_ai_filter.max_cloud_candidates then
                            local candidate = Candidate("baidu_cloud", segment._start, segment._end,
                                cloud_cand.value or cloud_cand, "")
                            candidate.quality = 900 + (cloud_ai_filter.max_cloud_candidates - i + 1) * 10
                            candidate.preedit = first_original_cand.preedit -- 保持原始预编辑文本
                            table.insert(ordered_candidates, candidate)
                            logger.info("添加云候选词: " .. (cloud_cand.value or cloud_cand))
                        end
                    end
                end

                if parsed_data.ai_candidates then
                    for i, ai_cand in ipairs(parsed_data.ai_candidates) do
                        if i <= cloud_ai_filter.max_ai_candidates then
                            -- local candidate = Candidate("ai_cloud", segment._start, segment._end,
                            --     ai_cand.value or ai_cand, "")
                            local candidate = Candidate("ai_cloud/" .. ai_cand.comment_name, segment._start, segment._end,
                                ai_cand.value or ai_cand, "")
                            candidate.quality = 950 + (cloud_ai_filter.max_ai_candidates - i + 1) * 10
                            candidate.preedit = first_original_cand.preedit -- 保持原始预编辑文本
                            table.insert(ordered_candidates, candidate)
                            logger.info("添加AI候选词: " .. (ai_cand.value or ai_cand))
                        end
                    end
                end

                if stream_result.is_final then
                    -- 最终数据，停止流式获取
                    context:set_property("get_cloud_stream", "stop")
                    logger.info("get_cloud_stream, 设置为stop")
                    -- 清空缓存数据，避免影响下次输入
                    clear_cloud_result_cache()
                    logger.info("云输入结果获取完成，停止流式获取，已清空缓存")
                end

            elseif stream_result and stream_result.status == "timeout" then
                -- 超时是正常的，继续等待
                logger.debug("云输入结果读取超时(正常) - 服务端可能还在处理")
                context:set_property("get_cloud_stream", "starting")

            elseif stream_result and stream_result.status == "error" then
                -- 连接错误，停止获取
                context:set_property("get_cloud_stream", "error")
                logger.info("get_cloud_stream, 设置为error")
                -- 连接错误时也清空缓存，避免使用不可靠的数据
                clear_cloud_result_cache()
                logger.error("云输入服务连接错误，停止流式获取，已清空缓存: " ..
                                 tostring(stream_result.error_msg))

            else
                -- 其他情况（无数据、未知状态等），尝试使用缓存数据
                logger.debug("未知的云输入结果状态或无数据，尝试使用缓存数据")

                local cached_data = get_cached_cloud_result(segment_input)
                if cached_data then
                    logger.info("使用缓存数据构建候选词")

                    -- 使用缓存的云候选词
                    if cached_data.cloud_candidates then
                        for i, cloud_cand in ipairs(cached_data.cloud_candidates) do
                            if i <= cloud_ai_filter.max_cloud_candidates then
                                local candidate = Candidate("baidu_cloud", segment._start, segment._end,
                                    cloud_cand.value or cloud_cand, "")
                                candidate.quality = 900 + (cloud_ai_filter.max_cloud_candidates - i + 1) * 10
                                candidate.comment = "☁📦" -- 添加缓存标识
                                candidate.preedit = first_original_cand.preedit
                                table.insert(ordered_candidates, candidate)
                                logger.info("添加缓存云候选词: " .. (cloud_cand.value or cloud_cand))
                            end
                        end
                    end

                    -- 使用缓存的AI候选词
                    if cached_data.ai_candidates then
                        for i, ai_cand in ipairs(cached_data.ai_candidates) do
                            if i <= cloud_ai_filter.max_ai_candidates then
                                local candidate = Candidate("ai_cloud/" .. ai_cand.comment_name, segment._start, segment._end,
                                    ai_cand.value or ai_cand, "")
                                candidate.quality = 950 + (cloud_ai_filter.max_ai_candidates - i + 1) * 10
                                candidate.comment = "🤖📦" -- 添加缓存标识
                                candidate.preedit = first_original_cand.preedit
                                table.insert(ordered_candidates, candidate)
                                logger.info("添加缓存AI候选词: " .. (ai_cand.value or ai_cand))
                            end
                        end
                    end
                else
                    logger.debug("没有可用的缓存数据")
                end
            end

            -- 为云输入候选词添加spans信息（用于光标跳转功能）
            if #ordered_candidates > 0 then
                local existing_spans = spans_manager.get_spans(context)
                if not existing_spans then
                    -- 从原生候选词中提取spans信息
                    local success = spans_manager.extract_and_save_from_candidate(context, first_original_cand, input,
                        "cloud_ai_filter_v2")
                    if success then
                        logger.info("为云输入候选词创建spans信息")
                    end
                end
            end

            -- 输出流式获取的候选词
            for _, candidate in ipairs(ordered_candidates) do
                yield(candidate)
            end
        end)
        if not ok then
            logger.error("云输入候选词处理异常: " .. tostring(err))
        end

        -- 输出原始候选词
        yield(first_original_cand)
        for cand in translation:iter() do
            yield(cand)
        end

        return
    end

    yield(first_original_cand)

    for _, cand in ipairs(long_candidates_table) do
        if cand ~= first_original_cand then -- 避免重复输出第一个候选词
            yield(cand)
        end
    end

    for _, cand in ipairs(no_long_candidates_table) do
        yield(cand)
    end

    for cand in translation:iter() do
        yield(cand)
    end
    logger.info("所有候选词输出完成.")

end

function cloud_ai_filter.fini(env)
    logger.info("云输入处理器结束运行")
end

return cloud_ai_filter
