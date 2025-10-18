-- AI对话分词器
-- 将 a:nihk 分割为两个段落：
-- 1. a: -> ai_talk 标签
-- 2. nihk -> abc 标签（正常拼音处理）
local logger_module = require("logger")
local debug_utils = require("debug_utils")

local logger = logger_module.create("ai_assistant_segmentor", {
    enabled = true,
    unique_file_log = false,
    log_level = "DEBUG"
})

-- 初始化时清空日志文件
logger.clear()

-- 模块级配置缓存
local ai_assistant_segmentor = {}
ai_assistant_segmentor.enabled = false
ai_assistant_segmentor.behavior = {}
ai_assistant_segmentor.chat_triggers = {}
ai_assistant_segmentor.reply_messages_preedits = {}
ai_assistant_segmentor.reply_tags = {}
ai_assistant_segmentor.chat_names = {}
ai_assistant_segmentor.clean_prefix_to_trigger = {}
-- 新增：回复输入快速查表（如 "<trigger>_reply:" -> "<trigger>"）
ai_assistant_segmentor.reply_inputs_to_trigger = {}
-- 新增：chat_triggers 的反向查表（如 "a:" -> "gpt"）
ai_assistant_segmentor.chat_triggers_reverse = {}

-- 读取配置的辅助函数，从config中读取并缓存到模块级变量
function ai_assistant_segmentor.update_current_config(config)
    logger.info("开始更新ai_assistant_segmentor模块配置")

    -- 读取 enabled 配置
    local enabled = config:get_bool("ai_assistant/enabled")
    ai_assistant_segmentor.keep_input_uncommit = config:get_bool("translator/keep_input_uncommit")
    ai_assistant_segmentor.enabled = enabled or false
    logger.info("AI助手启用状态: " .. tostring(ai_assistant_segmentor.enabled))

    -- 读取 behavior 配置
    ai_assistant_segmentor.behavior = {}
    ai_assistant_segmentor.behavior.commit_question = config:get_bool("ai_assistant/behavior/commit_question") or false
    ai_assistant_segmentor.behavior.auto_commit_reply = config:get_bool("ai_assistant/behavior/auto_commit_reply") or
                                                            false
    ai_assistant_segmentor.behavior.clipboard_mode = config:get_bool("ai_assistant/behavior/clipboard_mode") or false
    ai_assistant_segmentor.behavior.prompt_chat = config:get_string("ai_assistant/behavior/prompt_chat")

    logger.info("行为配置 - commit_question: " .. tostring(ai_assistant_segmentor.behavior.commit_question))
    logger.info("行为配置 - auto_commit_reply: " .. tostring(ai_assistant_segmentor.behavior.auto_commit_reply))
    logger.info("行为配置 - clipboard_mode: " .. tostring(ai_assistant_segmentor.behavior.clipboard_mode))
    logger.info("行为配置 - prompt_chat: " .. tostring(ai_assistant_segmentor.behavior.prompt_chat))

    -- 重新初始化所有配置表
    ai_assistant_segmentor.chat_triggers = {}
    ai_assistant_segmentor.reply_messages_preedits = {}
    ai_assistant_segmentor.reply_tags = {}
    ai_assistant_segmentor.chat_names = {}
    ai_assistant_segmentor.clean_prefix_to_trigger = {}
    -- 新增：重置回复输入查表
    ai_assistant_segmentor.reply_inputs_to_trigger = {}
    -- 新增：重置触发器反向查表
    ai_assistant_segmentor.chat_triggers_reverse = {}

    -- 获取 ai_prompts 配置项（新结构）
    local ai_prompts_config = config:get_map("ai_assistant/ai_prompts")
    if ai_prompts_config then
        local trigger_keys = ai_prompts_config:keys()
        logger.info("找到 " .. #trigger_keys .. " 个 ai_prompts 配置")

        -- 遍历 ai_prompts 中的每个助手条目
        for _, trigger_name in ipairs(trigger_keys) do
            local base_key = "ai_assistant/ai_prompts/" .. trigger_name

            local trigger_value = config:get_string(base_key .. "/chat_triggers")
            local reply_messages_preedit = config:get_string(base_key .. "/reply_messages_preedits")
            local chat_name = config:get_string(base_key .. "/chat_names")

            if trigger_value and #trigger_value > 0 then
                -- 正向映射：名称 -> 触发前缀，如 normal_ai_chat -> "ai:"
                ai_assistant_segmentor.chat_triggers[trigger_name] = trigger_value
                logger.info("聊天触发器 - " .. trigger_name .. ": " .. trigger_value)

                -- 反向映射：触发前缀 -> 名称，如 "ai:" -> normal_ai_chat
                ai_assistant_segmentor.chat_triggers_reverse[trigger_value] = trigger_name
                logger.debug("触发器反向映射 - " .. trigger_value .. " -> " .. trigger_name)

                -- 预处理：去掉末尾冒号，建立 clean_prefix -> 元信息 的映射
                local clean_prefix = trigger_value:gsub(":$", "")
                ai_assistant_segmentor.clean_prefix_to_trigger[clean_prefix] = {
                    trigger_name = trigger_name,
                    trigger_prefix = trigger_value,
                    chat_name = chat_name
                }
                logger.info("预处理触发器前缀 - " .. clean_prefix .. " -> " .. trigger_name)
            end

            if reply_messages_preedit and #reply_messages_preedit > 0 then
                ai_assistant_segmentor.reply_messages_preedits[trigger_name] = reply_messages_preedit
                logger.info("回复消息 - " .. trigger_name .. ": " .. reply_messages_preedit)

                -- 回复输入映射：例如 "normal_ai_chat_reply:" -> "normal_ai_chat"
                local reply_input_key = trigger_name .. "_reply:"
                ai_assistant_segmentor.reply_inputs_to_trigger[reply_input_key] = trigger_name
                logger.info("回复输入映射 - " .. reply_input_key .. " -> " .. trigger_name)
            end

            if chat_name and #chat_name > 0 then
                ai_assistant_segmentor.chat_names[trigger_name] = chat_name
                logger.info("聊天名称 - " .. trigger_name .. ": " .. chat_name)
            end
        end
    else
        logger.warn("未找到 ai_prompts 配置")
    end

    -- 不进行排序：假设配置不会产生多前缀同时匹配同一输入；若出现即为配置问题。

    logger.info("ai_assistant_segmentor模块配置更新完成")
end

function ai_assistant_segmentor.init(env)
    logger.info("AI对话分词器初始化完成")

    -- 配置更新由 cloud_input_processor 统一管理，无需在此处调用
    local config = env.engine.schema.config
    logger.info("等待 cloud_input_processor 统一更新配置")
end

function ai_assistant_segmentor.func(segmentation, env)
    local context = env.engine.context
    local input = context.input
    -- 保存到属性当中
    -- logger.debug("input: " .. input .. " #input: " .. tostring(#input))
    if ai_assistant_segmentor.keep_input_uncommit then
        if #input > 8 then
            context:set_property("input_string", input)
        elseif #input == 8 then
            local input_string = context:get_property("input_string")
            -- logger.debug("input_string: " .. input_string .. " #input_string" .. tostring(#input_string))
            if #context:get_property("input_string") == 9 then
                context:set_property("input_string", "")
            end
        end
    end

    -- 检查AI助手是否启用
    if not ai_assistant_segmentor.enabled then
        return true -- AI助手未启用，不处理
    end

    local segmentation_input = segmentation.input
    local confirmed_pos = segmentation:get_confirmed_position()
    local current_start = segmentation:get_current_start_position()
    local current_end = segmentation:get_current_end_position()

    logger.info("segmentation_input: " .. segmentation_input)
    local current_start_input = segmentation_input:sub(current_start + 1)
    logger.info("current_start_input: " .. current_start_input)

    -- 清空前面的分词,从这里开始进行分词
    -- debug_utils.print_segmentation_info(segmentation, logger)
    local trigger_name = context:get_property("current_ai_context")
    if segmentation.size == 2 and trigger_name then
        if current_start == 3 and current_end == 3 and segmentation.input:sub(-2) == ":c" then
            -- debug_utils.print_segmentation_info(segmentation, logger)
            logger.debug("进入清空历史聊天记录位置")
            local last_segment = segmentation:back()
            last_segment.tags = Set {"clear_chat_history"}
            last_segment._end = last_segment._end + 1
            return false
            -- debug_utils.print_segmentation_info(segmentation, logger)
        end
    end

    if confirmed_pos ~= 0 or current_start ~= 0 then
        -- 如果不是从头开始是分段处理,而是已经进行过一切选词了,则不再进本脚本的分词处理
        return true
    end

    -- 检查是否是AI回复消息（使用新的回复输入格式）
    logger.debug("检测AI回复输入: " .. segmentation_input)
    -- O(1) 直接查表，不再遍历
    local reply_trigger = ai_assistant_segmentor.reply_inputs_to_trigger[segmentation_input]
    if reply_trigger then
        logger.debug("检测到AI回复输入: " .. segmentation_input .. " (触发器: " .. reply_trigger .. ")")
        -- debug_utils.print_segmentation_info(segmentation, logger)
        local ai_reply_segment = Segment(0, #segmentation_input)
        ai_reply_segment.tags = Set {reply_trigger .. "_reply", "ai_reply"}
        if segmentation.size > 0 then
            segmentation:pop_back()
        end
        if segmentation:add_segment(ai_reply_segment) then
            logger.info("创建AI回复段落标签: " .. reply_trigger .. "_reply")
        else
            logger.error("失败: 创建AI回复段落标签: " .. reply_trigger .. "_reply")
        end
        -- debug_utils.print_segmentation_info(segmentation, logger)

        return false -- 处理完成, 其他所有分词器不再处理
    end

    -- 检查是否是提示触发符号, 例如"a"
    local prompt_chat = ai_assistant_segmentor.behavior.prompt_chat
    if segmentation_input == prompt_chat then
        logger.debug("segmentation_input == prompt: " .. segmentation_input)
        -- 收集所有以 prompt_chat 字母开头的触发器
        -- 创建提示段落
        local prompt_segment = Segment(0, #prompt_chat)
        prompt_segment.tags = Set {"ai_prompt", "abc"}

        segmentation:reset_length(0)
        segmentation:add_segment(prompt_segment)

        return false
    end

    do
        -- 触发器匹配：先精确(O(1))，否则前缀首匹配；配置应保证唯一
        if segmentation.size < 2 then
            local matched_prefix, matched_trigger_name, full_matched_prefix
            for prefix, t_name in pairs(ai_assistant_segmentor.chat_triggers_reverse) do
                if #segmentation_input >= #prefix and segmentation_input:sub(1, #prefix) == prefix then

                    if #segmentation_input == #prefix then
                        -- 如果进入这个分支则是完全匹配 ac: 后面没有其他内容,否则是后面还有内容.
                        full_matched_prefix = true
                    end
                    matched_prefix = prefix
                    matched_trigger_name = t_name
                    break -- 首次命中立即退出；若配置出现多重匹配应调整配置
                end
            end

            if matched_trigger_name then
                local ai_segment = Segment(0, #matched_prefix)
                ai_segment.tags = Set {matched_trigger_name, "ai_talk"}
                logger.debug(string.format("前缀触发匹配: %s -> %s", matched_prefix, matched_trigger_name))

                context:set_property("current_ai_context", matched_trigger_name)
                logger.info("设置AI上下文: " .. matched_trigger_name)

                segmentation:reset_length(0)
                if segmentation:add_segment(ai_segment) then
                    logger.debug("添加ai_talk分段成功")
                    if full_matched_prefix then
                        return false
                    else
                        segmentation:forward()
                        -- 首先打印segmentation里面的数据看看
                        -- debug_utils.print_segmentation_info(segmentation, logger)
                        if segmentation:get_current_start_position() == 3 and segmentation:get_current_end_position() ==
                            3 and segmentation.input:sub(-2) == ":c" then
                            logger.debug("进入清空历史聊天记录位置")
                            local last_segment = segmentation:back()
                            last_segment._end = last_segment._end + 1
                            last_segment.tags = Set {"clear_chat_history"}
                            return false
                        end
                    end

                else
                    logger.debug("添加ai_talk分段失败")
                end

            end
        end

    end

    return true -- 不能false啊,应该继续让后面的分词器继续处理呢!
end

function ai_assistant_segmentor.fini(env)
    logger.info("AI对话分词器结束运行")

end

return ai_assistant_segmentor
