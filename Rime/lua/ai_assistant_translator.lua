-- AI对话前缀翻译器
-- 处理 ai_talk 标签，显示 "〔AI对话〕" 提示
local logger_module = require("logger")
local debug_utils = require("debug_utils")
local logger = logger_module.create("ai_assistant_translator", {
    enabled = true,
    unique_file_log = false,
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

-- 模块级配置缓存
local ai_assistant_translator = {}
ai_assistant_translator.chat_triggers = {}
ai_assistant_translator.reply_messages_preedits = {}
ai_assistant_translator.chat_names = {}
ai_assistant_translator.reply_input_to_trigger = {}

-- 配置更新函数
function ai_assistant_translator.update_current_config(config)
    logger.info("开始更新ai_assistant_translator模块配置")

    -- 重新初始化所有配置表
    ai_assistant_translator.chat_triggers = {}
    ai_assistant_translator.reply_messages_preedits = {}
    ai_assistant_translator.chat_names = {}
    ai_assistant_translator.reply_input_to_trigger = {}

    -- 动态读取 ai_prompts 配置（新结构）
    local ai_prompts_config = config:get_map("ai_assistant/ai_prompts")
    if ai_prompts_config then
        local trigger_keys = ai_prompts_config:keys()
        logger.info("找到 " .. #trigger_keys .. " 个 ai_prompts 项")

        -- 遍历 ai_prompts 中的所有触发器条目
        for _, trigger_name in ipairs(trigger_keys) do
            local base_key = "ai_assistant/ai_prompts/" .. trigger_name

            local trigger_value = config:get_string(base_key .. "/chat_triggers")
            local reply_message_preedit = config:get_string(base_key .. "/reply_messages_preedits")
            local chat_name = config:get_string(base_key .. "/chat_names")

            if trigger_value and #trigger_value > 0 then
                ai_assistant_translator.chat_triggers[trigger_name] = trigger_value
                logger.info("AI触发器 - " .. trigger_name .. ": " .. trigger_value)
            end

            if reply_message_preedit and #reply_message_preedit > 0 then
                ai_assistant_translator.reply_messages_preedits[trigger_name] = reply_message_preedit
                logger.info("AI回复预编辑消息 - " .. trigger_name .. ": " .. reply_message_preedit)
            end

            if chat_name and #chat_name > 0 then
                ai_assistant_translator.chat_names[trigger_name] = chat_name
                logger.info("AI聊天名称 - " .. trigger_name .. ": " .. chat_name)
            end
        end
    else
        logger.warn("未找到 ai_prompts 配置")
    end

    -- 创建标签到触发器的反向映射
    for trigger_name, reply_messages_preedit in pairs(ai_assistant_translator.reply_messages_preedits) do
        ai_assistant_translator.reply_input_to_trigger[reply_messages_preedit] = trigger_name
    end

    logger.info("ai_assistant_translator模块配置更新完成")
end

function ai_assistant_translator.init(env)
    -- logger.clear()
    logger.info("AI对话翻译器初始化完成")

    -- 配置更新由 cloud_input_processor 统一管理，无需在此处调用
    local config = env.engine.schema.config
    logger.info("等待 cloud_input_processor 统一更新配置")

    local engine = env.engine
    local context = engine.context

    -- env.select_notifier = context.select_notifier:connect(function(context)
    --     if not context:is_composing() then
    --         -- logger.info("select_notifier, not context:is_composing()")
    --         -- local composition = context.composition
    --         -- local segmentation = composition:toSegmentation()
    --         -- debug_utils.print_segmentation_info(segmentation, logger)
    --         -- debug_utils.print_context_info(context, logger)
    --         local ai_talk_option = context:get_property("ai_talk_option")
    --         if ai_talk_option == "true" then
    --             logger.debug("选词通知中发现ai_talk_option开关, 重新输入input为'AI回复'这类触发词.")

    --             logger.debug("设置get_ai_stream属性开关start")
    --             context:set_property("get_ai_stream", "start")

    --             logger.debug("设置ai_talk_option属性开关false")
    --             context:set_property("ai_talk_option", "false")
    --         end
    --     end
    -- end)

    -- env.commit_notifier = context.commit_notifier:connect(function(context)
    --     local input = context.input

    --     -- 检查是否匹配任何AI触发器
    --     local matched_trigger = nil
    --     local matched_prefix = nil

    --     if env.ai_assistant_config and env.ai_assistant_config.chat_triggers then
    --         -- 使用已经读取的触发器配置
    --         for trigger_name, trigger_prefix in pairs(env.ai_assistant_config.chat_triggers) do
    --             if input:match("^" .. trigger_prefix:gsub("[%(%)%.%+%-%*%?%[%]%^%$%%]", "%%%1") .. ".") then
    --                 matched_trigger = trigger_name
    --                 matched_prefix = trigger_prefix
    --                 logger.info("检测到AI对话输入，触发器: " .. trigger_name .. " (" .. trigger_prefix .. ")")
    --                 break
    --             end
    --         end
    --     end

    --     if matched_trigger then
    --         logger.info("检测到AI对话输入: " .. matched_trigger)

    --         local ok, err = pcall(function()
    --             local commit_text = context:get_commit_text()
    --             logger.info("给服务端发送对话请求: " .. commit_text .. " (类型: " .. matched_trigger .. ")")

    --             -- 读取最新消息（丢弃积压的旧消息，保留最新的有用消息）
    --             local flushed_bytes = tcp_socket.flush_ai_socket_buffer()
    --             if flushed_bytes and flushed_bytes > 0 then
    --                 logger.info("清理了积压的AI消息: " .. flushed_bytes .. " 字节")
    --             else
    --                 logger.info("无积压的AI消息需要处理")
    --             end

    --             -- 清理上次的候选词
    --             local current_content = context:get_property("ai_replay_stream")
    --             if current_content ~= "" and current_content ~= "等待回复..." then
    --                 context:set_property("ai_replay_stream", "等待回复...")
    --             end

    --             -- 发送聊天消息，包含对话类型信息
    --             tcp_socket.send_chat_message(commit_text, matched_trigger)

    --             local ai_talk_option = context:get_property("ai_talk_option")
    --             if ai_talk_option ~= "true" then
    --                 logger.debug("设置ai_talk_option属性开关true")
    --                 context:set_property("ai_talk_option", "true")
    --             end
    --         end)
    --         if not ok then
    --             logger.error("AI对话请求处理出错: " .. tostring(err))
    --         end

    --     end
    -- end)
end

function ai_assistant_translator.func(input, segment, env)
    logger.info("ai_talk_translator.lua开始执行")

    -- 检查是否是任何AI相关标签（统一处理逻辑）
    local matched_reply_tag = nil
    local matched_trigger = nil
    local reply_message = nil
    local preedit_pre = nil
    local is_prefix_display = false

    -- debug_utils.print_segment_info(segment, logger)
    -- 检查所有配置的AI触发器标签（无需遍历 chat_triggers，依赖 segment 的标签与上下文）
    if ai_assistant_translator.chat_triggers then
        local context = env.engine.context
        local trigger_name = context:get_property("current_ai_context")
        -- 仅当该分词段带有 ai_talk 标签时才认为是触发器前缀段

        if segment:has_tag("ai_talk") then

            -- 确认该 trigger 存在于配置表中
            if trigger_name and ai_assistant_translator.chat_triggers[trigger_name] then
                matched_reply_tag = trigger_name
                matched_trigger = trigger_name
                is_prefix_display = true -- 这是前缀显示
                local trigger_prefix = ai_assistant_translator.chat_triggers[trigger_name]
                -- 从配置中获取聊天名称，如果没有则使用触发器前缀
                reply_message = ai_assistant_translator.chat_names[trigger_name] or (trigger_prefix .. " AI助手")
                logger.info("检测到AI触发器标签: " .. trigger_name .. " (前缀显示)")
            end
        elseif segment:has_tag("clear_chat_history") then
            -- 生成一个指定候选词,然后返回
            local candidate = Candidate("clear_chat_history", segment.start, segment._end, "清空对话记录", "")
            yield(candidate)
            logger.info("清空对话记录: " .. trigger_name)
            return
        end
    end

    -- -- 检查所有配置的AI回复标签
    -- if not matched_reply_tag and ai_assistant_translator.chat_triggers then
    --     for trigger_name, chat_trigger in pairs(ai_assistant_translator.chat_triggers) do
    --         if segment:has_tag(trigger_name .. "_reply") then
    --             matched_reply_tag = trigger_name .. "_reply"
    --             matched_trigger = trigger_name
    --             preedit_pre = ai_assistant_translator.reply_messages_preedits[trigger_name]
    --             is_prefix_display = false -- 这是回复显示
    --             -- 从配置中获取回复预编辑消息
    --             logger.info("检测到AI回复标签: " .. matched_reply_tag .. " (触发器: " .. matched_trigger ..
    --                             ")")
    --             break
    --         end
    --     end
    -- end

    -- 检查所有配置的AI回复标签
    if not matched_reply_tag and ai_assistant_translator.chat_triggers then
        if segment:has_tag("ai_reply") then
            local matched_reply_tag_set = segment.tags - Set {"ai_reply"}
            matched_reply_tag = next(matched_reply_tag_set)
            logger.debug("matched_reply_tag: " .. matched_reply_tag)
            matched_trigger = matched_reply_tag:gsub("_reply$", "")
            logger.debug("matched_trigger: " .. matched_trigger)
            preedit_pre = ai_assistant_translator.reply_messages_preedits[matched_trigger]
            logger.debug("preedit_pre: " .. preedit_pre)
            is_prefix_display = false -- 这是回复显示
            -- 从配置中获取回复预编辑消息
            logger.info("检测到AI回复标签: " .. matched_reply_tag .. " (触发器: " .. matched_trigger .. ")")
        end
    end

    -- 只处理AI相关标签的段落
    if not matched_reply_tag then
        return
    end

    -- 前缀显示的处理（显示触发器前缀候选词）
    if is_prefix_display then
        logger.info("处理AI触发器前缀段落: " .. input)
        local candidate = Candidate(matched_trigger, segment.start, segment._end, reply_message, "")
        candidate.quality = 1000

        -- 为触发器前缀候选词设置 preedit（可选，通常前缀显示不需要特殊的 preedit）
        -- candidate.preedit = reply_message  -- 前缀显示通常不需要额外的 preedit

        yield(candidate)
        logger.info("生成AI触发器前缀候选词: " .. reply_message)
        return
    end

    -- AI回复标签的处理（流式获取和显示AI回复, 进入到这里说明一定是AI回复的分支）
    logger.info("处理AI回复段落，标签: " .. matched_reply_tag .. "，触发器: " .. matched_trigger)
    local context = env.engine.context

    -- 检查是否停止流式获取
    if context:get_property("get_ai_stream") == "stop" then
        logger.info("get_ai_stream == stop, 直接获取历史记录.")

        local current_content = context:get_property("ai_replay_stream")
        if not current_content or current_content == "" then
            current_content = "等待回复..."
        end

        local candidate = Candidate(matched_reply_tag, segment.start, segment._end, current_content, "")
        candidate.quality = 1000
        candidate.preedit = preedit_pre

        yield(candidate)
        -- if context:confirm_current_selection() then
        --     logger.info("确认当前AI回复候选词")
        -- end
        return
    end

    -- 执行流式获取AI回复
    logger.debug("read_latest_from_ai_socket执行")
    local stream_result = tcp_socket.read_latest_from_ai_socket()

    -- 根据优化后的返回结构判断是否继续获取数据
    if stream_result and stream_result.status == "success" and stream_result.data then
        local stream_data = stream_result.data
        logger.debug("成功获取到AI数据，状态: " .. stream_result.status .. " stream_data.is_final: " ..
                         tostring(stream_data.is_final))

        if stream_data.error then
            -- 发生错误，停止获取
            context:set_property("get_ai_stream", "idle")
            logger.debug("发生错误，停止流式获取: " .. tostring(stream_data.error))
        elseif stream_data.is_final then
            -- 最终数据，停止获取
            context:set_property("get_ai_stream", "stop")

            logger.debug("intercept_select_key: 1")
            context:set_property("intercept_select_key", "1")
            logger.debug("收到最终数据(is_final=true)，停止流式获取")
        else
            -- 非最终数据，继续获取
            context:set_property("get_ai_stream", "start")
            logger.debug("继续流式获取，内容长度: " .. (stream_data.content and #stream_data.content or 0))
        end

        -- 更新AI回复内容
        if stream_data.content and stream_data.content ~= "" then
            context:set_property("ai_replay_stream", stream_data.content)
            logger.debug("更新AI回复内容: " .. stream_data.content)
        end

    elseif stream_result and stream_result.status == "timeout" then
        -- 超时是正常的，继续等待，不停止流式获取
        logger.debug("AI服务超时(正常) - 服务端可能还没发送数据，继续保持流式获取状态")
        context:set_property("get_ai_stream", "start")

    elseif stream_result and stream_result.status == "no_data" then
        -- 接收到数据但没有有效消息行，继续等待
        logger.debug("接收到数据但没有有效消息，继续保持流式获取状态")
        context:set_property("get_ai_stream", "start")

    elseif stream_result and stream_result.status == "error" then
        -- 连接错误，停止获取
        context:set_property("get_ai_stream", "idle")
        logger.error("AI服务连接错误，停止流式获取: " .. tostring(stream_result.error_msg))

    else
        -- 其他未知情况，保持当前状态不变
        logger.debug("未知的stream_result状态，保持当前流式获取状态")
    end

    logger.info("进入ai_reply标签重新生成候选词")

    -- 获取当前保存的AI回复内容，如果没有则显示等待提示
    local current_content = context:get_property("ai_replay_stream")
    logger.debug("current_content: " .. current_content)
    if current_content == "" then
        current_content = "等待回复..."
        logger.info("等待回复: " .. current_content)
    end

    -- 生成候选词
    local candidate = Candidate(matched_reply_tag, segment.start, segment._end, current_content, "")
    candidate.quality = 1000
    if current_content and preedit_pre then
        logger.debug("使用current_content生成候选词: " .. current_content .. " preedit_pre: " .. preedit_pre)
    else
        logger.error("current_content or preedit_pre: nil")
    end

    candidate.preedit = preedit_pre

    yield(candidate)

end

function ai_assistant_translator.fini(env)
    logger.info("AI对话翻译器结束运行")

    -- 清理选词通知器，参考 smart_cursor_processor.lua 的写法
    if env.commit_notifier then
        env.commit_notifier:disconnect()
    end

end

return ai_assistant_translator
