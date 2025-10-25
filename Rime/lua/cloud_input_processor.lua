-- 引入日志工具模块
local logger_module = require("logger")

local logger = logger_module.create("cloud_input_processor", {
    enabled = true,
    unique_file_log = false,
    log_level = "DEBUG"
})

-- 初始化时清空日志文件
logger.clear()

-- 引入文本切分模块
local debug_utils = require("debug_utils")

-- 安全加载模块，防止脚本不存在时出错
local function safe_require(module_name)
    local ok, module = pcall(require, module_name)
    if ok then
        logger.debug("成功加载模块: " .. module_name)
        return module
    else
        logger.warn("加载模块失败: " .. module_name .. " - " .. tostring(module))
        return nil
    end
end

local text_splitter = safe_require("text_splitter")

-- 引入TCP同步模块
local tcp_socket
local tcp_ok, tcp_err = pcall(function()
    tcp_socket = require("tcp_socket_sync")
end)
if not tcp_ok then
    logger.error("加载 tcp_socket_sync 失败: " .. tostring(tcp_err))
end

-- 返回值常量定义
local kRejected = 0 -- 表示按键被拒绝
local kAccepted = 1 -- 表示按键已被处理
local kNoop = 2 -- 表示按键未被处理,继续传递给下一个处理器

local cloud_input_processor = {}

-- 模块级别的 schema 跟踪变量
cloud_input_processor.last_schema_id = nil

-- 配置更新函数
function cloud_input_processor.update_current_config(config)
    logger.debug("重新加载AI助手配置")

    -- 读取分隔符配置
    cloud_input_processor.delimiter = config:get_string("speller/delimiter"):sub(1, 1) or " "
    logger.debug("当前分隔符: " .. cloud_input_processor.delimiter)

    -- 读取云转换触发符号配置
    cloud_input_processor.cloud_convert_symbol = config:get_string("translator/cloud_convert_symbol") or "Return"
    logger.debug("云转换触发符号: " .. cloud_input_processor.cloud_convert_symbol)

    -- 读取英文模式符号配置
    cloud_input_processor.english_mode_symbol = config:get_string("translator/english_mode_symbol") or "`"
    logger.debug("英文模式符号: " .. cloud_input_processor.english_mode_symbol)

    -- 读取原始英文分隔符配置
    cloud_input_processor.rawenglish_delimiter_after = config:get_string("translator/rawenglish_delimiter_after") or "`"
    cloud_input_processor.rawenglish_delimiter_before = config:get_string("translator/rawenglish_delimiter_before") or
                                                            "`"
    logger.debug("原始英文后分隔符: " .. cloud_input_processor.rawenglish_delimiter_after)
    logger.debug("原始英文前分隔符: " .. cloud_input_processor.rawenglish_delimiter_before)

    -- 初始化配置对象
    cloud_input_processor.ai_assistant_config = {}
    cloud_input_processor.ai_assistant_config.chat_triggers = {}
    cloud_input_processor.ai_assistant_config.chat_names = {}
    cloud_input_processor.ai_assistant_config.reply_messages_preedits = {}
    cloud_input_processor.ai_assistant_config.prefix_to_reply = {}

    -- 读取 enabled 配置
    cloud_input_processor.ai_assistant_config.enabled = config:get_bool("ai_assistant/enabled")
    logger.debug("AI助手启用状态: " .. tostring(cloud_input_processor.ai_assistant_config.enabled))

    -- 读取 behavior 配置
    cloud_input_processor.ai_assistant_config.behavior = {}

    cloud_input_processor.ai_assistant_config.behavior.commit_question = config:get_bool(
        "ai_assistant/behavior/commit_question") or false
    cloud_input_processor.ai_assistant_config.behavior.strip_chat_prefix = config:get_bool(
        "ai_assistant/behavior/strip_chat_prefix") or false
    cloud_input_processor.ai_assistant_config.behavior.add_reply_prefix = config:get_bool(
        "ai_assistant/behavior/add_reply_prefix") or false
    cloud_input_processor.ai_assistant_config.behavior.auto_commit_reply = config:get_bool(
        "ai_assistant/behavior/auto_commit_reply") or false
    cloud_input_processor.ai_assistant_config.behavior.clipboard_mode = config:get_bool(
        "ai_assistant/behavior/clipboard_mode") or false
    cloud_input_processor.ai_assistant_config.behavior.prompt_chat = config:get_string(
        "ai_assistant/behavior/prompt_chat")
    cloud_input_processor.ai_assistant_config.behavior.auto_commit_reply_send_key = config:get_string(
        "ai_assistant/behavior/auto_commit_reply_send_key")
    cloud_input_processor.ai_assistant_config.behavior.after_question_send_key = config:get_string(
        "ai_assistant/behavior/after_question_send_key")

    logger.debug("行为配置 - commit_question: " ..
                     tostring(cloud_input_processor.ai_assistant_config.behavior.commit_question))
    logger.debug("行为配置 - auto_commit_reply: " ..
                     tostring(cloud_input_processor.ai_assistant_config.behavior.auto_commit_reply))
    logger.debug("行为配置 - clipboard_mode: " ..
                     tostring(cloud_input_processor.ai_assistant_config.behavior.clipboard_mode))
    logger.debug("行为配置 - prompt_chat: " ..
                     tostring(cloud_input_processor.ai_assistant_config.behavior.prompt_chat))
    logger.debug("行为配置 - auto_commit_reply_send_key: " ..
                     tostring(cloud_input_processor.ai_assistant_config.behavior.auto_commit_reply_send_key))
    logger.debug("行为配置 - after_question_send_key: " ..
                     tostring(cloud_input_processor.ai_assistant_config.behavior.after_question_send_key))

    -- 动态读取 ai_prompts 配置（新结构）
    local ai_prompts_config = config:get_map("ai_assistant/ai_prompts")
    if ai_prompts_config then
        -- 获取所有键名
        local trigger_keys = ai_prompts_config:keys()
        logger.debug("找到 " .. #trigger_keys .. " 个 ai_prompts 配置")

        -- 遍历配置中的所有触发器条目
        for _, trigger_name in ipairs(trigger_keys) do
            local base_key = "ai_assistant/ai_prompts/" .. trigger_name

            local trigger_value = config:get_string(base_key .. "/chat_triggers")
            local reply_message = config:get_string(base_key .. "/reply_messages_preedits")
            local chat_name = config:get_string(base_key .. "/chat_names")

            if trigger_value and #trigger_value > 0 then
                cloud_input_processor.ai_assistant_config.chat_triggers[trigger_name] = trigger_value
                logger.debug("云输入触发器 - " .. trigger_name .. ": " .. trigger_value)
            end

            if chat_name and #chat_name > 0 then
                cloud_input_processor.ai_assistant_config.chat_names[trigger_name] = chat_name
                logger.debug("聊天名称 - " .. trigger_name .. ": " .. chat_name)
            end

            if reply_message and #reply_message > 0 then
                cloud_input_processor.ai_assistant_config.reply_messages_preedits[trigger_name] = reply_message
                logger.debug("云输入回复消息 - " .. trigger_name .. ": " .. reply_message)
            end
        end
    else
        logger.warn("未找到 ai_prompts 配置")
    end

    -- 创建触发器前缀到回复消息的映射
    for trigger, prefix in pairs(cloud_input_processor.ai_assistant_config.chat_triggers) do
        local reply_message = cloud_input_processor.ai_assistant_config.reply_messages_preedits[trigger]
        if reply_message then
            cloud_input_processor.ai_assistant_config.prefix_to_reply[prefix] = reply_message
        end
    end

    -- 读取菜单配置
    local ok_menu, err_menu = pcall(function()
        cloud_input_processor.ai_assistant_config.page_size = config:get_int("menu/page_size")
        cloud_input_processor.ai_assistant_config.alternative_select_keys = config:get_string(
            "menu/alternative_select_keys")
    end)
    if ok_menu then
        logger.debug("page_size: " .. tostring(cloud_input_processor.ai_assistant_config.page_size))
        logger.debug("alternative_select_keys: " ..
                         tostring(cloud_input_processor.ai_assistant_config.alternative_select_keys))

        -- 从alternative_select_keys中截取前page_size个字符
        if cloud_input_processor.ai_assistant_config.alternative_select_keys and
            cloud_input_processor.ai_assistant_config.page_size then
            cloud_input_processor.ai_assistant_config.alternative_select_keys =
                cloud_input_processor.ai_assistant_config.alternative_select_keys:sub(1,
                    cloud_input_processor.ai_assistant_config.page_size)
            logger.debug("截取后的alternative_select_keys: " ..
                             tostring(cloud_input_processor.ai_assistant_config.alternative_select_keys))
        end
    else
        logger.error("获取菜单配置失败: " .. tostring(err_menu))
        -- 设置默认值
        cloud_input_processor.ai_assistant_config.page_size = 5
        cloud_input_processor.ai_assistant_config.alternative_select_keys = "123456789"
        -- 截取默认值
        cloud_input_processor.ai_assistant_config.alternative_select_keys =
            cloud_input_processor.ai_assistant_config.alternative_select_keys:sub(1,
                cloud_input_processor.ai_assistant_config.page_size)
        logger.debug("使用默认菜单配置 - page_size: " .. cloud_input_processor.ai_assistant_config.page_size ..
                         ", alternative_select_keys: " ..
                         cloud_input_processor.ai_assistant_config.alternative_select_keys)
    end

    logger.debug("AI助手配置更新完成")
end


local property_update_table = {}
function cloud_input_processor.update_context_property(property_name, property_value)
    -- 将这里要更新的属性保存到全局变量中
    property_update_table[property_name] = property_value
    logger.debug("保存待更新属性到table: " .. property_name .. " = " .. tostring(property_value))
end

-- 计算候选词中汉字的数量
local function count_chinese_characters(text)
    -- 使用utf8库计算中文字符数量
    local count = 0
    for pos, code in utf8.codes(text) do
        -- 中文字符的Unicode范围：
        -- 基本汉字区：0x4E00-0x9FFF
        -- 扩展A区：0x3400-0x4DBF
        -- 其他常用中文符号区间
        if (code >= 0x4E00 and code <= 0x9FFF) or (code >= 0x3400 and code <= 0x4DBF) then
            count = count + 1
        end
    end

    return count
end

-- 从script_text末尾移除指定数量的音节
local function remove_syllables_from_end(script_text, syllable_count, delimiter)
    if syllable_count <= 0 then
        return script_text
    end

    -- 按分隔符分割script_text
    local parts = {}
    for part in script_text:gmatch("[^" .. delimiter .. "]+") do
        table.insert(parts, part)
    end

    -- 如果要移除的音节数量大于等于总数，返回空字符串
    if syllable_count >= #parts then
        return ""
    end

    -- 移除末尾的指定数量音节
    local result_parts = {}
    for i = 1, #parts - syllable_count do
        table.insert(result_parts, parts[i])
    end

    -- 重新组合，保持原有的分隔符
    return table.concat(result_parts, delimiter)
end

-- 构建最终的上屏文本使用preedit版本
local function build_commit_text_preedit(preedit_text, candidate_text, delimiter, chat_name)

    -- 首先处理preedit_text，去除最后一个"‸"符号及其后面的内容
    local cleaned_preedit_text = preedit_text
    -- 使用简单的find查找‸符号位置，然后截取到该位置之前
    local cursor_pos = preedit_text:find("‸")
    if cursor_pos then
        cleaned_preedit_text = preedit_text:sub(1, cursor_pos - 1)
        logger.debug("去除光标符号及后续内容，原文本: '" .. preedit_text .. "', 处理后: '" ..
                         cleaned_preedit_text .. "'")
    end

    -- 检查并提取chat_trigger_name前缀
    local prefix = ""
    local actual_preedit_text = cleaned_preedit_text

    if chat_name and cleaned_preedit_text:sub(1, #chat_name) == chat_name then
        prefix = chat_name
        actual_preedit_text = cleaned_preedit_text:sub(#chat_name + 1)
        logger.debug("提取出前缀: '" .. chat_name .. "', 剩余preedit_text: '" .. actual_preedit_text .. "'")
    end

    logger.debug("原始preedit_text: '" .. actual_preedit_text .. "'")
    logger.debug("候选词文本: '" .. candidate_text .. "'")

    --[[ 匹配方式: 从candidate_text最后一个字符开始, 不行啊, 空格如何作为进入英文模式的判断的话, 如果英文当中也有空格将会导致出错.
    对preedit_text进行遍历, 如果遇到一个
    
    ]]

    -- 工作变量
    local temp_preedit_text = actual_preedit_text
    local in_english_mode = false -- 是否在英文模式中

    -- 将候选词转换为字符数组，方便从后往前遍历
    local candidate_chars = {}
    for pos, code in utf8.codes(candidate_text) do
        table.insert(candidate_chars, utf8.char(code))
    end

    -- 从后往前遍历候选词中的每个字符
    for i = #candidate_chars, 1, -1 do
        local char = candidate_chars[i]
        logger.debug("处理字符: '" .. char .. "' (位置: " .. i .. ")")

        -- 检查是否是英文分隔符
        if char == cloud_input_processor.rawenglish_delimiter_after then
            -- 遇到后分隔符，进入英文模式
            in_english_mode = true
            logger.debug("遇到英文后分隔符，进入英文模式")
            -- 移除preedit_text末尾的一个字符（对应这个分隔符）
            if #temp_preedit_text > 0 then
                temp_preedit_text = temp_preedit_text:sub(1, -2)
                logger.debug("移除分隔符，剩余preedit_text: '" .. temp_preedit_text .. "'")
            end
            goto continue
        elseif char == cloud_input_processor.rawenglish_delimiter_before then
            -- 遇到前分隔符，退出英文模式
            in_english_mode = false
            logger.debug("遇到英文前分隔符，退出英文模式")
            -- 移除preedit_text末尾的一个字符（对应这个分隔符）
            if #temp_preedit_text > 0 then
                temp_preedit_text = temp_preedit_text:sub(1, -2)
                logger.debug("移除分隔符，剩余preedit_text: '" .. temp_preedit_text .. "'")
            end
            goto continue
        end

        if in_english_mode then
            -- 英文模式：一个字符对应preedit_text中的一个字符
            if #temp_preedit_text > 0 then
                temp_preedit_text = temp_preedit_text:sub(1, -2)
                logger.debug("英文模式：移除一个字符，剩余preedit_text: '" .. temp_preedit_text .. "'")
            end
        else
            -- 中文模式：判断字符类型
            local char_code = utf8.codepoint(char)
            local is_chinese = (char_code >= 0x4E00 and char_code <= 0x9FFF) or
                                   (char_code >= 0x3400 and char_code <= 0x4DBF)
            local is_punctuation =
                string.match(char, "[%p%s]") or (char_code >= 0x3000 and char_code <= 0x303F) or -- CJK符号和标点
                    (char_code >= 0xFF00 and char_code <= 0xFFEF) -- 全角ASCII

            if is_chinese or is_punctuation then
                -- 中文字符或标点符号：移除一个音节
                if i == #candidate_chars then
                    -- 最后一个字符：移除最后一个音节（不包含分隔符）
                    local last_delimiter_pos = temp_preedit_text:find(delimiter .. "[^" .. delimiter .. "]*$")
                    if last_delimiter_pos then
                        temp_preedit_text = temp_preedit_text:sub(1, last_delimiter_pos)
                        logger.debug("移除最后一个音节（无分隔符），剩余preedit_text: '" ..
                                         temp_preedit_text .. "'")
                    else
                        -- 如果找不到分隔符，说明只有一个音节，清空
                        temp_preedit_text = ""
                        logger.debug("只有一个音节，清空preedit_text")
                    end
                else
                    -- 非最后字符：移除一个音节, 不包含分隔符
                    -- 'ys ld dr hw jx uk ni zi ji , vs jm tm jw hr hh fu '
                    local temp_preedit_text_strip = temp_preedit_text:gsub("%s+$", "") -- 先去掉末尾空白
                    local last_delimiter_pos = temp_preedit_text_strip:match(".*()%s") -- 找到“最后一个空格”的位置
                    if last_delimiter_pos then
                        temp_preedit_text = temp_preedit_text:sub(1, last_delimiter_pos)
                        logger.debug("移除最后一个音节，剩余preedit_text: '" .. temp_preedit_text .. "'")
                    else
                        -- 如果找不到分隔符，查找最后一个rawenglish_delimiter_after符号并截取到该位置（保留符号）
                        local pos = temp_preedit_text:find(cloud_input_processor.english_mode_symbol .. "[^" ..
                                                               cloud_input_processor.english_mode_symbol .. "]*$")
                        if pos then
                            temp_preedit_text = temp_preedit_text:sub(1, pos +
                                #cloud_input_processor.english_mode_symbol - 1)
                            logger.debug("截取到最后一个英文分隔符位置，剩余preedit_text: '" ..
                                             temp_preedit_text .. "'")
                        else
                            temp_preedit_text = ""
                            logger.debug("找不到分隔符和英文分隔符，清空preedit_text")
                        end
                    end
                end
            else
                -- 其他字符（如英文字母、数字等）：移除preedit_text末尾一个字符
                if #temp_preedit_text > 0 then
                    temp_preedit_text = temp_preedit_text:sub(1, -2)
                    logger.debug("移除一个字符，剩余preedit_text: '" .. temp_preedit_text .. "'")
                end
            end
        end

        ::continue::
    end

    local processed_preedit_text = temp_preedit_text
    logger.debug("最终处理后的preedit_text: '" .. processed_preedit_text .. "'")

    -- 组合最终文本
    local final_text
    if processed_preedit_text == "" then
        final_text = prefix .. candidate_text
    else
        final_text = prefix .. processed_preedit_text .. candidate_text
    end

    logger.debug("最终上屏文本: '" .. final_text .. "'")
    return final_text
end

-- 构建最终的上屏文本
local function build_commit_text(script_text, candidate_text, delimiter, chat_trigger_name)

    -- 检查并提取chat_trigger_name前缀
    local prefix = ""
    local actual_script_text = script_text

    if chat_trigger_name and script_text:sub(1, #chat_trigger_name) == chat_trigger_name then
        prefix = chat_trigger_name
        actual_script_text = script_text:sub(#chat_trigger_name + 1)
        logger.info("提取出前缀: '" .. prefix .. "', 剩余script_text: '" .. actual_script_text .. "'")
    end

    logger.info("原始script_text: '" .. actual_script_text .. "'")
    logger.info("候选词文本: '" .. candidate_text .. "'")

    --[[ 应该对actual_script_text进行遍历 ]]

    -- 工作变量
    local temp_script_text = actual_script_text
    local in_english_mode = false -- 是否在英文模式中

    -- 将候选词转换为字符数组，方便从后往前遍历
    local candidate_chars = {}
    for pos, code in utf8.codes(candidate_text) do
        table.insert(candidate_chars, utf8.char(code))
    end

    -- 从后往前遍历候选词中的每个字符
    for i = #candidate_chars, 1, -1 do
        local char = candidate_chars[i]
        logger.debug("处理字符: '" .. char .. "' (位置: " .. i .. ")")

        -- 检查是否是英文分隔符
        if char == cloud_input_processor.rawenglish_delimiter_after then
            -- 遇到后分隔符，进入英文模式
            in_english_mode = true
            logger.debug("遇到英文后分隔符，进入英文模式")
            -- 移除script_text末尾的一个字符（对应这个分隔符）
            if #temp_script_text > 0 then
                temp_script_text = temp_script_text:sub(1, -2)
                logger.debug("移除分隔符，剩余script_text: '" .. temp_script_text .. "'")
            end
            goto continue
        elseif char == cloud_input_processor.rawenglish_delimiter_before then
            -- 遇到前分隔符，退出英文模式
            in_english_mode = false
            logger.debug("遇到英文前分隔符，退出英文模式")
            -- 移除script_text末尾的一个字符（对应这个分隔符）
            if #temp_script_text > 0 then
                temp_script_text = temp_script_text:sub(1, -2)
                logger.debug("移除分隔符，剩余script_text: '" .. temp_script_text .. "'")
            end
            goto continue
        end

        if in_english_mode then
            -- 英文模式：一个字符对应script_text中的一个字符
            if #temp_script_text > 0 then
                temp_script_text = temp_script_text:sub(1, -2)
                logger.debug("英文模式：移除一个字符，剩余script_text: '" .. temp_script_text .. "'")
            end
        else
            -- 中文模式：判断字符类型
            local char_code = utf8.codepoint(char)
            local is_chinese = (char_code >= 0x4E00 and char_code <= 0x9FFF) or
                                   (char_code >= 0x3400 and char_code <= 0x4DBF)
            local is_punctuation =
                string.match(char, "[%p%s]") or (char_code >= 0x3000 and char_code <= 0x303F) or -- CJK符号和标点
                    (char_code >= 0xFF00 and char_code <= 0xFFEF) -- 全角ASCII

            if is_chinese or is_punctuation then
                -- 中文字符或标点符号：移除一个音节
                if i == #candidate_chars then
                    -- 最后一个字符：移除最后一个音节（不包含分隔符）
                    local last_delimiter_pos = temp_script_text:find(delimiter .. "[^" .. delimiter .. "]*$")
                    if last_delimiter_pos then
                        temp_script_text = temp_script_text:sub(1, last_delimiter_pos)
                        logger.debug("移除最后一个音节（无分隔符），剩余script_text: '" ..
                                         temp_script_text .. "'")
                    else
                        -- 如果找不到分隔符，说明只有一个音节，清空
                        temp_script_text = ""
                        logger.debug("只有一个音节，清空script_text")
                    end
                else
                    -- 非最后字符：移除一个音节, 不包含分隔符
                    -- 'ys ld dr hw jx uk ni zi ji , vs jm tm jw hr hh fu '
                    local temp_script_text_strip = temp_script_text:gsub("%s+$", "") -- 先去掉末尾空白
                    local last_delimiter_pos = temp_script_text_strip:match(".*()%s") -- 找到“最后一个空格”的位置
                    if last_delimiter_pos then
                        temp_script_text = temp_script_text:sub(1, last_delimiter_pos)
                        logger.debug("移除最后一个音节，剩余script_text: '" .. temp_script_text .. "'")
                    else
                        -- 如果找不到分隔符，查找最后一个rawenglish_delimiter_after符号并截取到该位置（保留符号）
                        local pos = temp_script_text:find(cloud_input_processor.english_mode_symbol .. "[^" ..
                                                              cloud_input_processor.english_mode_symbol .. "]*$")
                        if pos then
                            temp_script_text = temp_script_text:sub(1,
                                pos + #cloud_input_processor.english_mode_symbol - 1)
                            logger.debug("截取到最后一个英文分隔符位置，剩余script_text: '" ..
                                             temp_script_text .. "'")
                        else
                            temp_script_text = ""
                            logger.debug("找不到分隔符和英文分隔符，清空script_text")
                        end
                    end
                end
            else
                -- 其他字符（如英文字母、数字等）：移除script_text末尾一个字符
                if #temp_script_text > 0 then
                    temp_script_text = temp_script_text:sub(1, -2)
                    logger.debug("移除一个字符，剩余script_text: '" .. temp_script_text .. "'")
                end
            end
        end

        ::continue::
    end

    local processed_script_text = temp_script_text
    logger.info("最终处理后的script_text: '" .. processed_script_text .. "'")

    -- 组合最终文本
    local final_text
    if processed_script_text == "" then
        final_text = prefix .. candidate_text
    else
        final_text = prefix .. processed_script_text .. candidate_text
    end

    logger.info("最终上屏文本: '" .. final_text .. "'")
    return final_text
end


-- 获取所有segment选择的候选词, 当前使用的函数
local function all_segmentation_selected_candidate(key_repr, chat_trigger, env, segmentation)
    local engine = env.engine
    local context = engine.context
    -- 检查当前按键是否为选词键或空格键
    local is_select_key = false
    local select_key_index = 0
    -- 如果是ai_talk标签的segment, 则需要判断是不是将要上屏, 如果要上屏,则进行拦截后处理
    local first_segment = segmentation:get_at(0)
    local last_segment = segmentation:back()
    -- local menu = last_segment.menu
    -- logger.debug("menu:candidate_count(): " .. tostring(menu:candidate_count()))

    if key_repr == "space" then
        -- 空格键按照选词键1处理
        is_select_key = true
        select_key_index = 1
        logger.debug("检测到空格键，按选词键1处理 (索引: " .. select_key_index .. ")")
    else
        -- 直接查找字符在选词键字符串中的位置,这里就是如果第一个选词键是1,则这里面按下1就是select_key_index为1
        select_key_index = string.find(cloud_input_processor.ai_assistant_config.alternative_select_keys, key_repr, 1,
            true)
        if select_key_index then
            is_select_key = true
            logger.debug("检测到选词键: " .. key_repr .. " (索引: " .. select_key_index .. ")")
        end
    end

    if is_select_key then

        local menu = last_segment.menu
        if last_segment and menu then
            -- 检查menu是否为空以及选词索引是否在有效范围内
            if not menu:empty() and select_key_index <= cloud_input_processor.ai_assistant_config.page_size then

                -- 获取即将上屏的候选词
                -- Calculate candidate index across pages
                local candidate_count = menu:candidate_count()
                local page_size = cloud_input_processor.ai_assistant_config.page_size
                local candidate_index = select_key_index - 1

                local page_index = math.floor((candidate_count - 1) / page_size)
                local candidates_before_current_page = page_index * page_size
                local current_page_count = candidate_count - candidates_before_current_page
                if current_page_count <= 0 then
                    current_page_count = page_size
                end

                if select_key_index > current_page_count then
                    logger.debug("选词索引超出当前页可用候选数: " .. select_key_index .. " > " ..
                                     current_page_count)
                    return kNoop
                end

                candidate_index = candidates_before_current_page + (select_key_index - 1)

                if candidate_index >= candidate_count then
                    logger.debug("候选索引超出范围: " .. candidate_index .. " >= " .. candidate_count)
                    return kNoop
                end

                local candidate = last_segment:get_candidate_at(candidate_index)
                if candidate then

                    -- 检查选词后是否会完成完整输入（上屏）
                    -- 通过检查context状态和segment状态来判断

                    -- 判断是否为最后一个未确认的segment，且选择后会导致上屏
                    local is_last_candidate = (candidate._end == #context.input)
                    if is_last_candidate then
                        -- 在这里添加一个分支: 判断候选词的类型是不是我自己设置的: "clear_chat_history", 如果是: 则直接取消上屏, 并发送socket消息.
                        if candidate.type == "clear_chat_history" then
                            -- 发送聊天消息，包含对话类型信息, command_value应该是assitant_id, assitant_id也就是chat_trigger
                            tcp_socket.sync_with_server(env, false, nil, "clear_chat_history", chat_trigger)

                            -- 拦截按键, 清空当前context中的内容. 应该根据配置清空控制是否清空,或者正常上屏. 如果上屏则应该发送回车.
                            logger.debug("clear_chat_history: 清空候选词不上屏, context:clear()")
                            context:clear()
                            return kAccepted
                        end

                        logger.debug("选词将完成上屏操作，拦截按键并发送AI消息")
                        local candidate_text = candidate.text
                        logger.info("候选词文本: " .. candidate_text)

                        -- debug_utils.print_segmentation_info(segmentation, logger)
                        -- 对所有的segment进行遍历, 获取每一段的get_selected_candidate

                        -- 拼接除最后一个segment外的所有候选词文本
                        local prefix_text_with_first = "" -- 包含第一段segment的结果
                        local prefix_text_without_first = "" -- 不包含第一段segment的结果

                        for i = 0, segmentation.size - 2 do -- 排除最后一个segment
                            local seg = segmentation:get_at(i)
                            if seg then
                                local cand = seg:get_selected_candidate()
                                if cand then
                                    -- 包含第一段的结果
                                    prefix_text_with_first = prefix_text_with_first .. cand.text

                                    -- 不包含第一段的结果（跳过索引0）
                                    if i > 0 then
                                        prefix_text_without_first = prefix_text_without_first .. cand.text
                                    end

                                    logger.info("segment[" .. i .. "] cand.text: " .. cand.text)
                                else
                                    logger.info("segment[" .. i .. "] 没有选中的候选词")
                                end
                            end
                        end

                        -- 记录拼接结果
                        logger.info("拼接的前缀文本: " .. prefix_text_without_first)
                        logger.info("拼接的前缀文本: " .. prefix_text_without_first)
                        local all_selected_candidate_with_first = prefix_text_with_first .. candidate_text
                        local all_selected_candidate_without_first = prefix_text_without_first .. candidate_text

                        -- 记录两个结果
                        logger.info("包含第一段的全部候选词文本: " .. all_selected_candidate_with_first)
                        logger.info("不包含第一段的全部候选词文本: " ..
                                        all_selected_candidate_without_first)

                        local ok, result = pcall(function()

                            -- 读取最新消息（丢弃积压的旧消息，保留最新的有用消息）
                            local flushed_bytes = tcp_socket.flush_ai_socket_buffer()
                            if flushed_bytes and flushed_bytes > 0 then
                                logger.debug("清理了积压的AI消息: " .. flushed_bytes .. " 字节")
                            else
                                logger.debug("无积压的AI消息需要处理")
                            end

                            -- 清理上次的候选词
                            local current_content = context:get_property("ai_replay_stream")
                            if current_content ~= "" and current_content ~= "等待回复..." then
                                context:set_property("ai_replay_stream", "等待回复...")
                            end

                            -- 设置一个属性说明当前将会进入AI提问轮的标识, 在哪里关闭呢 ?
                            context:set_property("start_ai_question", "1")

                            -- 如果当前不是start状态则设置为start状态
                            local get_ai_stream = context:get_property("get_ai_stream")
                            if get_ai_stream ~= "start" then
                                logger.debug("设置get_ai_stream属性开关start")
                                context:set_property("get_ai_stream", "start")
                            end

                            if cloud_input_processor.ai_assistant_config.behavior.commit_question then
                                local response_key
                                if cloud_input_processor.ai_assistant_config.behavior.after_question_send_key then
                                    response_key = cloud_input_processor.ai_assistant_config.behavior
                                                       .after_question_send_key
                                end
                                tcp_socket.send_chat_message(all_selected_candidate_without_first, chat_trigger,
                                    response_key) -- 正常输入换行
                                -- 再判断strip_chat_prefix为true或者false,如果为true,则清空并且重新上屏字符串
                                if cloud_input_processor.ai_assistant_config.behavior.strip_chat_prefix then

                                    logger.debug("context:clear()")
                                    context:clear()

                                    engine:commit_text(all_selected_candidate_without_first)
                                    return kAccepted
                                else
                                    -- 正常上屏操作, 不去除前缀的话,就会正常的向后推动,变成一个普通的上屏操作
                                    logger.debug("未设置strip_chat_prefix, 不需要删除前缀，直接上屏: " ..
                                                     all_selected_candidate_with_first)
                                    logger.debug("context:clear()")
                                    context:clear()

                                    engine:commit_text(all_selected_candidate_with_first)
                                    return kAccepted
                                end

                            else
                                -- 发送聊天消息，包含对话类型信息
                                tcp_socket.send_chat_message(all_selected_candidate_without_first, chat_trigger, false)
                                -- 拦截按键, 清空当前context中的内容. 应该根据配置清空控制是否清空,或者正常上屏. 如果上屏则应该发送回车.
                                logger.debug("context:clear()")
                                context:clear()
                                return kAccepted
                            end
                        end)

                        if ok then
                            -- 执行成功，返回pcall内部函数的返回值
                            return result
                        else
                            -- 执行失败，记录错误但不拦截按键
                            logger.error("AI对话请求处理出错: " .. tostring(result))
                            return kNoop
                        end
                    end

                else
                    logger.warn("无法获取候选词对象")
                end
            else
                logger.debug("菜单为空或选词索引超出范围: " .. select_key_index .. " > " ..
                                 (menu:candidate_count() or 0))
            end
        else
            logger.debug("没有有效的segment或menu")
        end
    end
end

local function set_cloud_convert_flag(context)
    -- 这部分代码时检测输入的字符长度，通过检测中间有几个分隔符实现
    -- 检查当前是否正在组词状态（即用户正在输入但还未确认）
    local is_composing = context:is_composing()
    local preedit = context:get_preedit()
    local preedit_text = preedit.text
    -- 这里不需要考虑已经确认的部分,确认的部分不会出现在preedit_text中.
    -- 移除光标符号和后续的prompt内容
    local clean_text = preedit_text:gsub("‸.*$", "") -- 从光标符号开始删除到结尾
    logger.debug("当前预编辑文本: " .. clean_text)
    local _, count = string.gsub(clean_text, cloud_input_processor.delimiter, cloud_input_processor.delimiter)
    logger.debug("当前输入内容分隔符数量: " .. count)
    -- local has_punct = has_punctuation(input)

    -- 触发状态改成,当数如字符超过4个,或者有标点且超过2个:
    if is_composing and count >= 3 then
        logger.debug("当前正在组词状态,检测到分隔符数量达到3,触发云输入提示")
        -- 只在值真正需要改变时才设置
        -- 先获取当前选项的值，避免不必要的更新
        logger.debug("当前云输入提示标志: " .. context:get_property("cloud_convert_flag"))

        if context:get_property("cloud_convert_flag") == "0" then
            logger.debug("云输入提示标志为 0, 设置为 1")
            context:set_property("cloud_convert_flag", "1")
            logger.debug("cloud_convert_flag 已设置为 1")

        end

    else
        -- 如果不在组词状态或没有达到触发条件,则重置提示选项
        logger.debug("当前不在组词状态或未达到触发条件,云输入提示已重置")
        if context:get_property("cloud_convert_flag") == "1" then
            context:set_property("cloud_convert_flag", "0")
            logger.debug("cloud_convert_flag 已设置为 0")

        end
    end
end

function cloud_input_processor.init(env)
    -- 获取输入法引擎和上下文   
    local config = env.engine.schema.config
    local current_schema_id = env.engine.schema.schema_id

    --  fixed 设置一个变量
    -- context:set_property只能设置字符串类型
    env.engine.context:set_property("cloud_convert_flag", "0")
    env.engine.context:set_property("rawenglish_prompt", "0")
    cloud_input_processor.handle_keys = text_splitter.handle_keys

    logger.debug("云输入处理器初始化完成")
end

-- 按键处理器函数
-- 负责监听按键事件,判断是否应该触发翻译器
function cloud_input_processor.func(key, env)
    local engine = env.engine
    local context = engine.context

    local segmentation = context.composition:toSegmentation()
    local input = context.input
    local config = env.engine.schema.config
    local key_repr = key:repr()
    logger.debug("测试虚拟按键: " .. key_repr)


    -- 检查Alt+F11按键的处理
    if key_repr == "Alt+F14" then
        -- logger.debug("执行到Alt+F11分支")
        if context:get_property("get_ai_stream") == "start" then
            logger.debug("get_ai_stream==start, 触发重新刷新候选词: ")
            if context.input == "" then
                local current_ai_context = context:get_property("current_ai_context")
                context.input = current_ai_context .. "_reply:"
                logger.debug("设置AI回复输入: " .. current_ai_context)
            end
            context:refresh_non_confirmed_composition()
            return kNoop
        elseif context:get_property("get_ai_stream") == "stop" then
            logger.debug("set_property get_ai_stream=idle")
            context:set_property("get_ai_stream", "idle")
            if cloud_input_processor.ai_assistant_config.behavior.auto_commit_reply then
                logger.debug("get_ai_stream==stop, 自动上屏: ")

                -- logger.debug("确认当前AI回复候选词")

                -- 在这里忘记考虑多行的可能性了,如果多行的话,这个地方会出现bug,所以还是应该用下面的那个.
                -- 所以用confirm_current_selection面对多行可能会出现问题
                local key = KeyEvent("space")
                engine:process_key(key)
                -- logger.debug("发送space键自动上屏")

                -- if context:confirm_current_selection() then
                --     -- 记录一个属性发送回车
                --     context:set_property("send_return_key", "1")
                -- else
                --     logger.debug("失败在确认当前AI回复候选词")
                -- end
            end
            return kNoop
        else
            logger.debug("get_ai_stream==idle")
            return kNoop
        end
    end

    -- 检查并应用待更新的属性
    if next(property_update_table) ~= nil then
        logger.debug("发现待更新的属性，开始应用到context中")
        for property_name, property_value in pairs(property_update_table) do
            logger.debug("更新属性: " .. property_name .. " = " .. tostring(property_value))
            context:set_property(property_name, tostring(property_value))
        end
        -- 清空属性更新表
        property_update_table = {}
        logger.debug("属性更新完成，已清空property_update_table")
    end

    if context:get_property("should_intercept_key_release") == "1" then
        -- 检查是否需要拦截Release+Shift_L按键
        if key_repr == "Release+Shift_L" or key_repr == "Release+Shift_R" then
            logger.debug("拦截Release+Shift_L按键（由于之前处理了Shift+组合键）")
            -- 清除标志，避免影响后续操作
            context:set_property("should_intercept_key_release", "0")
            return kAccepted
        end
    end

    if key_repr == "Alt+F13" then
        if context:get_property("get_cloud_stream") == "starting" then
            logger.debug("get_cloud_stream==starting, 触发重新刷新云输入候选词: ")
            context:refresh_non_confirmed_composition()

        else
            logger.debug("get_cloud_stream:  " .. context:get_property("get_cloud_stream"))
        end
        return kAccepted
    end

    local is_composing = context:is_composing()
    if not key or not context:is_composing() then
        return kNoop
    end

    -- AI回复上屏处理分支
    if context:get_property("intercept_select_key") == "1" then

        if key_repr == "space" or key_repr == "1" then
            logger.debug("进入分支 get_property intercept_select_key: 1")

            logger.debug("set_property intercept_select_key: 0")
            context:set_property("intercept_select_key", "0")

            if context:get_property("input_string") ~= "" then
                context:set_property("input_string", "")
                logger.info("清空context:set_property input_string")
            end

            -- 判断是不是直接一个段落, 内容中是否存在换行符.
            local commit_text = context:get_commit_text()
            logger.debug("commit_text: " .. commit_text)

            -- 记录一个属性发送一个按键
            logger.debug("auto_commit_reply_send_key: " ..
                             cloud_input_processor.ai_assistant_config.behavior.auto_commit_reply_send_key)
            if cloud_input_processor.ai_assistant_config.behavior.auto_commit_reply_send_key ~= "" and
                cloud_input_processor.ai_assistant_config.behavior.auto_commit_reply_send_key ~= "none" then
                context:set_property("send_key",
                    cloud_input_processor.ai_assistant_config.behavior.auto_commit_reply_send_key)
            end

            if commit_text and commit_text:find("\n") then
                logger.debug("commit_text 中存在换行符")
                -- 拦截按键, 清空当前context中的内容.
                context:clear()
                logger.debug("context:clear()结束")
                -- 使用TCP通信发送粘贴命令到Python服务端（跨平台通用）
                if tcp_socket then
                    logger.debug("🍴 通过TCP发送粘贴命令到Python服务端 (intercept模式)")
                    -- 如果获取input中的文本呢? 
                    if cloud_input_processor.ai_assistant_config.behavior.add_reply_prefix then
                        local script_text = context:get_script_text()
                        engine:commit_text(script_text)
                        -- engine:commit_text(script_text .. commit_text)
                        context:clear()
                    end

                    local paste_success
                    logger.debug("send_key: " .. context:get_property("send_key"))
                    if context:get_property("send_key") ~= "" then
                        paste_success = tcp_socket.sync_with_server(env, true, true, "button",
                            "paste_then_" .. context:get_property("send_key"))
                        context:set_property("send_key", "")
                    else
                        paste_success = tcp_socket.sync_with_server(env, false, false, "button", "paste")
                    end

                    if paste_success then
                        logger.debug("✅ 粘贴命令发送成功 (intercept模式)")
                        return kAccepted
                    else
                        logger.error("❌ 粘贴命令发送失败 (intercept模式)")
                        return kNoop
                    end

                else
                    logger.warn("⚠️ TCP模块未加载，无法发送粘贴命令 (intercept模式)")
                    return kNoop
                end
                return kAccepted

            else
                logger.debug("commit_text 中不存在换行符")
                -- 如果获取input中的文本呢? 
                if cloud_input_processor.ai_assistant_config.behavior.add_reply_prefix then
                    local script_text = context:get_script_text()
                    -- engine:commit_text(script_text)
                    engine:commit_text(script_text .. commit_text)
                    context:clear()
                else
                    engine:commit_text(commit_text)
                    context:clear()
                end

                logger.debug("send_key: " .. context:get_property("send_key"))
                if context:get_property("send_key") ~= "" then
                    tcp_socket.sync_with_server(env, true, true, "button", context:get_property("send_key"))
                    context:set_property("send_key", "")
                else
                    tcp_socket.sync_with_server(env, true, true)
                end

                -- return kNoop

            end

            return kAccepted

        end

    end

    -- 如果是ai_talk标签的segment, 则需要判断是不是将要上屏, 如果要上屏,则进行拦截后处理
    local first_segment = segmentation:get_at(0)
    local last_segment = segmentation:back()

    -- 英文模式豁免, 就是这段引起的bug, 也就是当前面有 ai_talk标签的时候一定会进入这段代码中
    logger.debug("property: rawenglish_prompt: " .. context:get_property("rawenglish_prompt"))
    if first_segment:has_tag("ai_talk") and context:get_property("rawenglish_prompt") == "0" then
        logger.debug("first_segment.tags: ai_talk")
        -- for element, _ in pairs(first_segment.tags) do
        --     logger.debug("first_segment.tags: " .. element)
        -- end
        local tag = first_segment.tags - Set {"ai_talk"}
        -- 遍历Set，由于只有一个元素，第一次循环就会得到结果
        local tag_chat_trigger
        for element, _ in pairs(tag) do
            tag_chat_trigger = element
            logger.debug("tag_chat_trigger: " .. tag_chat_trigger)
            break
        end

        local result = all_segmentation_selected_candidate(key_repr, tag_chat_trigger, env, segmentation)
        logger.debug("all_segmentation_selected_candidate result: " .. tostring(result))
        if result then
            return result
        end

        -- debug_utils.print_segmentation_info(segmentation, logger)

        -- -- 这个方式不太好,放弃这个方法，换一个更好的方法。处理AI会话是否要进行传输等操作
        -- local result = handle_ai_chat_selection(key_repr, tag_chat_trigger, env, last_segment)
        -- logger.debug("handle_ai_chat_selection result: " .. tostring(result))
        -- if result then
        --     return result
        -- end

    end

    -- 使用 pcall 捕获所有可能的错误
    local success, result = pcall(function()

        if #input <= 1 then
            logger.debug("input为1, 不判断直接退出")
            return kNoop
        end

        -- 如果输入的按键是一个反引号,则判断这个反引号是不是一个和前边的反引号配对的闭合单引号
        -- 如果是则直接将当前第一个候选项上屏.
        logger.debug("")
        logger.debug("=== 开始分析lua/cloud_input_processor.lua ===")
        logger.debug("当前按键: " .. key_repr)
        logger.debug("当前input: " .. input)

        logger.debug("context:get_property:rawenglish_prompt " .. context:get_property("rawenglish_prompt"))

        -- 首先打印seg的信息
        -- 使用debug_utils打印Segmentation信息
        -- debug_utils.print_segmentation_info(segmentation, logger)
        logger.debug("当前英文模式: " .. context:get_property("rawenglish_prompt"))

        if context:get_property("rawenglish_prompt") == "1" then
            if key_repr:match("^Release%+") then
                logger.debug("反引号状态下跳过按键事件: " .. key_repr)
                return kAccepted
            end

            logger.debug("key_repr: " .. key_repr)
            if cloud_input_processor.handle_keys[key_repr] then
                logger.debug("处于反引号状态，将按键转为普通字符: " .. key_repr)

                -- 如果是Shift+XXX按键，设置属性用于拦截后续的Release+Shift_L
                if key_repr:match("^Shift%+") then
                    context:set_property("should_intercept_key_release", "1")
                    logger.debug("检测到Shift+组合键，设置拦截按键释放标志")
                end

                -- 将按键对应的字符添加到输入中
                local char_to_add = cloud_input_processor.handle_keys[key_repr]
                -- 如果添加英文字母没有影响,但是
                context:push_input(char_to_add)

                -- 返回 kAccepted 表示我们已经处理了这个按键
                return kAccepted
            end
        end

        logger.debug("=== 结束分析lua/cloud_input_processor.lua ===")
        logger.debug("")

        -- 设置云输入法表示标

        set_cloud_convert_flag(context)

        -- 检查当前按键是否为预设的触发键
        if key:repr() == cloud_input_processor.cloud_convert_symbol and context:get_property("cloud_convert_flag") ==
            "1" then
            logger.debug("触发云输入处理cloud_convert")
            -- debug_utils.print_segmentation_info(segmentation, logger)
            context:set_property("cloud_convert", "1")
            logger.debug("cloud_convert添加之后")
            context:refresh_non_confirmed_composition()
            -- debug_utils.print_segmentation_info(segmentation, logger)

            -- 设置拦截标志，用于拦截后续的按键释放事件
            context:set_property("should_intercept_key_release", "1")
            logger.debug("设置拦截按键释放标志")

            -- 返回已处理,阻止其他处理器处理这个按键

            return kAccepted
        end

        logger.debug("没有处理该按键, 返回kNoop")
        return kNoop
    end)

    -- 处理错误情况
    if not success then
        local error_message = tostring(result)
        logger.error("云输入处理器发生错误: " .. error_message)

        -- 记录详细的错误信息用于调试
        logger.error("错误堆栈信息: " .. debug.traceback())

        -- 在发生错误时,安全地返回 kNoop,让其他处理器继续工作
        return kNoop
    end

    -- 成功执行,返回处理结果
    logger.debug("云输入处理器执行成功, 返回值: " .. tostring(result))
    return result or kNoop
end

function cloud_input_processor.fini(env)
    logger.debug("云输入处理器结束运行")
end

return cloud_input_processor
