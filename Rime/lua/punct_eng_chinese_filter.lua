-- lua/punct_eng_chinese_filter.lua
-- 将候选项当中的英文标点符号改成中文标点符号
-- 引入日志工具模块
local logger_module = require("logger")
-- 引入文本切分模块
local text_splitter = require("text_splitter")
-- 引入spans管理模块
local spans_manager = require("spans_manager")

-- 创建当前模块的日志记录器
local logger = logger_module.create("punct_eng_chinese_filter", {
    enabled = true, -- 启用日志以便测试
    unique_file_log = false, -- 启用日志以便测试
    log_level = "DEBUG"
})
-- 清空日志文件
logger.clear()

local punct_eng_chinese_filter = {}

-- 模块级别的配置变量
punct_eng_chinese_filter.delimiter = ""
punct_eng_chinese_filter.ai_reply_tags = {}
punct_eng_chinese_filter.ai_chat_triggers = {}
punct_eng_chinese_filter.cloud_convert_symbol = ""

-- 配置更新函数
function punct_eng_chinese_filter.update_current_config(config)
    if not config then
        logger.error("无效的配置对象")
        return
    end

    punct_eng_chinese_filter.delimiter = config:get_string("speller/delimiter"):sub(1, 1) or " "
    logger.info("更新分隔符: " .. punct_eng_chinese_filter.delimiter)

    -- 读取云转换触发符号配置
    punct_eng_chinese_filter.cloud_convert_symbol = config:get_string("translator/cloud_convert_symbol")
    logger.info("云转换触发符号: " .. punct_eng_chinese_filter.cloud_convert_symbol)

    -- 重新初始化AI标签
    punct_eng_chinese_filter.ai_reply_tags = {}
    punct_eng_chinese_filter.ai_chat_triggers = {}

    -- 读取 AI 助手触发器配置，动态生成回复标签（新结构 ai_prompts）
    local ai_prompts_map = config:get_map("ai_assistant/ai_prompts")
    if ai_prompts_map then
        local trigger_keys = ai_prompts_map:keys()
        local tag_count = 0
        for _, trigger_name in ipairs(trigger_keys) do
            -- 保存触发器标签（使用条目名作为标签）
            punct_eng_chinese_filter.ai_chat_triggers[trigger_name] = true
            logger.info("保存AI聊天触发器标签: " .. trigger_name)

            -- 动态生成回复标签（触发器名称 + "_reply"）
            local reply_tag = trigger_name .. "_reply"
            punct_eng_chinese_filter.ai_reply_tags[reply_tag] = true
            tag_count = tag_count + 1
            logger.info("动态生成AI回复标签: " .. reply_tag)
        end
        logger.info("AI标签生成完成，共 " .. tostring(tag_count) .. " 个触发器和回复标签")
    else
        logger.info("未找到 ai_prompts 配置")
    end
end

function punct_eng_chinese_filter.init(env)
    -- 初始化时清空日志文件
    logger.info("标点英中文过滤器初始化完成")

    -- 配置更新由 cloud_input_processor 统一管理，无需在此处调用
    logger.info("等待 cloud_input_processor 统一更新配置")
end

function punct_eng_chinese_filter.func(translation, env)

    local engine = env.engine
    local context = engine.context

    local input = context.input

    -- 判断是否存在标点符号或者长度超过设定值,如果是在seg后面添加prompt说明
    local segment = ""

    -- 在segment后面添加prompt
    local composition = context.composition
    if (not composition:empty()) then
        -- 获得队尾的 Segment 对象
        segment = composition:back()
        if segment then
            -- logger.info("当前cloud_convert_prompt状态: ".. tostring(context:get_option("cloud_convert_prompt")))

            -- 定义两种提示文本
            local cloud_symbol_display = punct_eng_chinese_filter.cloud_convert_symbol
            if punct_eng_chinese_filter.cloud_convert_symbol == "Shift+Return" then
                cloud_symbol_display = "⇧+回车"
            elseif punct_eng_chinese_filter.cloud_convert_symbol == "Return" then
                cloud_symbol_display = "回车"
            end
            local cloud_prompt_text = "    ▶ [" .. cloud_symbol_display .. " AI转换]  "
            local cloud_prompt_error = "    ▶[服务端未连接] "
            local cloud_prompt_starting = "    ▶[云端获取中] "
            local cloud_prompt_stop = "    ▶[云端转换完成] "

            local rawenglish_prompt_text = "    ▶ [英文模式]  "
            local search_move_prompt = "    ▶ [搜索模式]  "
            local search_move_prompt_char = "    ▶ [搜索模式:%s]  "

            -- 获取两个状态
            local search_move = context:get_option("search_move")
            local rawenglish_prompt = context:get_property("rawenglish_prompt")
            local cloud_convert_flag = context:get_property("cloud_convert_flag")
            local get_cloud_stream = context:get_property("get_cloud_stream")

            -- 判断显示哪个提示（ search_move优先级更高, rawenglish_prompt第二）
            if search_move then
                -- 在搜索模式应该怎么办？走到这里一定是重新分词了, 那么如果输入了a, 光标应该跳到了a的位置, 这时应该在提示词当中添加a.
                -- 为什么不能在 processor 里面添加？因为光标跳转触发的重新分词，并没有输入新字母。
                local add_search_move_str = context:get_property("search_move_str")
                segment.prompt = string.format(" ▶ [搜索模式:%s] ", add_search_move_str)
                logger.info("更新搜索模式提示: " .. segment.prompt)

            elseif rawenglish_prompt == "1" then
                -- rawenglish_prompt 优先级最高
                if segment.prompt ~= rawenglish_prompt_text then
                    segment.prompt = rawenglish_prompt_text
                    logger.info("设置反引号提示: " .. rawenglish_prompt_text)
                end
            elseif get_cloud_stream == "error" then
                if segment.prompt ~= cloud_prompt_error then
                    segment.prompt = cloud_prompt_error
                    logger.info("设置云输入提示: " .. cloud_prompt_error)
                end
            elseif get_cloud_stream == "starting" then
                if segment.prompt ~= cloud_prompt_starting then
                    segment.prompt = cloud_prompt_starting
                    logger.info("设置云输入提示: " .. cloud_prompt_starting)
                end
            elseif get_cloud_stream == "stop" then
                if segment.prompt ~= cloud_prompt_stop then
                    segment.prompt = cloud_prompt_stop
                    logger.info("设置云输入提示: " .. cloud_prompt_stop)
                end
            elseif cloud_convert_flag == "1" then
                -- 这个地方不对, 首先应该确定get_cloud_stream在不同情况下的值, 应该是在cloud_convert_flag 为1 的时候会进来.
                -- 在这的状态应该是只要是足够发起AI对话的通知即可
                -- 只有在 rawenglish_prompt 为 0 时才显示 cloud_convert_flag 的提示
                if segment.prompt ~= cloud_prompt_text then
                    logger.info("segment.prompt: " .. segment.prompt .. " cloud_prompt_text: " .. cloud_prompt_text)
                    segment.prompt = cloud_prompt_text
                    logger.info("设置云输入提示: " .. cloud_prompt_text)
                end
            end
        end

    end

    -- 自动检查并清除过期的spans信息
    -- spans_manager.auto_clear_check(context, context.input)

    -- 使用 pcall 捕获所有可能的错误
    local success, error_msg = pcall(function()
        logger.info("标点符号过滤器开始处理")

        local count = 0 -- 用于计数，限制最多处理4个候选词
        -- 遍历所有候选词并进行标点符号替换
        local punch_flag = false -- 是否存在标点符号
        local ai_flag = false
        local ai_chat = false
        for cand in translation:iter() do

            count = count + 1
            -- 当 count 等于 1 的时候，这个时候判断是否有标点符号，如果没有则后面不用判断了。
            if count == 1 then
                -- 判断候选词类型，进行豁免。
                -- 检查是否为AI回复类型，如果是则豁免标点符号替换

                -- 对于 a:nihk 这样的内容，是否应该跳过标点符号替换呢？标点符号不需要跳过替换，但是spans信息,应该能够得到正确的处理
                logger.info("监测是否ai回复标签, cand.type: " .. cand.type)
                if cand.type and punct_eng_chinese_filter.ai_reply_tags[cand.type] then
                    logger.info("候选词类型为AI回复标签，豁免标点符号替换: " .. cand.type)
                    -- 对AI回复类型的候选词直接输出，不进行标点符号替换
                    ai_flag = true

                elseif cand.type and punct_eng_chinese_filter.ai_chat_triggers[cand.type] then
                    -- logger.info("候选词类型为AI聊天触发器标签,应该标点符号替换,但是不保存spans信息.")
                    ai_chat = true
                    logger.info("ai_chat: true")

                else
                    logger.info("cand.text: " .. cand.text)
                    if cand.text and text_splitter.has_punctuation_no_rawenglish(cand.text, logger) then
                        punch_flag = true
                        logger.info("punch_flag: true")
                        -- 对于标点符号替换，等于说直接生成了新的候选词,所以需要保存spans信息.
                        -- 普通候选词的spans信息可以直接通过candidate:spans()获取
                        -- rawenglish_combo类型的spans信息已经在rawenglish_translator中保存到spans_manager了
                    end
                end
            end

            -- 检查输入是否包含反引号标签
            local new_text = ""
            local cand_text = cand.text
            local cand_type = cand.type
            local cand_comment = cand.comment
            if not ai_flag and punch_flag and count < 10 then
                logger.info("进入not ai_reply_flag and punch_flag and count < 10")

                -- 反引号组合候选词rawenglish_combo
                if cand_comment:match("^chinese_pos:") then
                    logger.info("候选词为chinese_pos, 使用反引号替换")
                    -- 我应该在cand里面附带上自己的信息, 也就是哪部分是通过rawenglish合并进来的, lua/script_rawenglish_translator.lua, 可以放在comment当中, 然后在这里再删除掉即可
                    -- 将反引号索引段的信息保存到了cand.comment
                    -- 当有多个索引的时候,应该判断

                    logger.info("cand.comment: " .. cand.comment .. " cand_text: " .. cand_text)
                    local chinese_pos = cand.comment
                    logger.info("chinese_pos: " .. chinese_pos)
                    new_text = text_splitter.replace_punct_skip_pos(cand_text, chinese_pos, logger)
                else
                    logger.info("候选词不是chinese_pos ,按照原来的处理即可, 也就是没有反引号.")
                    new_text = text_splitter.replace_punct(cand_text)

                    -- 从候选词中提取spans信息并保存, 对于at:这类候选词,也是到了这个地方,但是abc为什么会产生这样的spans信息呢? 
                    -- 不对, 应该是两个segment,所以分成了两段. 
                    local spans = cand:spans()

                    logger.info(" cand.type: " .. cand.type .. " cand.start: " .. cand.start .. " cand._end: " ..
                                    cand._end)
                    if cand.text then
                        local vertices = spans.vertices
                        logger.info("vertices: " .. table.concat(vertices, ", "))
                        logger.info("cand.text: " .. cand.text)
                        if not ai_chat then
                            spans_manager.save_spans(context, vertices, input, "punct_eng_chinese_filter")
                        end

                    end

                    -- 这里的标点符号的候选词是不是应该在abc创建的时候是有spans信息的?
                end

                -- if not segment:has_tag("rawenglish_combo") then
                --     logger.info("没有包含rawenglish标签,按照原来的处理即可")
                --     new_text = text_splitter.replace_punct(cand_text)
                -- else
                --     logger.info("包含rawenglish标签,反引号之内的部分需要跳过标点符号替换")
                --     -- 我应该在cand里面附带上自己的信息, 也就是哪部分是通过rawenglish合并进来的, lua/script_rawenglish_translator.lua, 可以放在comment当中, 然后在这里再删除掉即可
                --     -- 将反引号索引段的信息保存到了cand.comment
                --     -- 当有多个索引的时候,应该判断

                --     logger.info("cand.comment: " .. cand.comment .. " cand_text: " .. cand_text)
                --     local chinese_pos = cand.comment
                --     new_text = text_splitter.replace_punct_skip_pos(cand_text, chinese_pos, logger)
                -- end

                logger.info("标点替换: " .. cand_text .. " -> " .. new_text)
                -- 根据文档，使用Candidate构造方法创建新候选项
                -- Candidate(type, start, end, text, comment)
                logger.info("cand_type: " .. cand_type)
                if cand_type == "web_cloud" then
                    cand_comment = "   [云输入]"
                elseif cand_type == "ai_cloud" then
                    cand_comment = "   [AI识别]"
                elseif cand_type:sub(1, #"ai_cloud") == "ai_cloud" then
                    local suffix = cand_type:match("([^/]+)$")
                    cand_comment = "   [" .. suffix .. "]"
                elseif cand_type == "rawenglish_combo" then
                    cand_comment = ""
                end
                local new_cand = Candidate(cand.type or "punct_converted", -- 保持原有类型或标记为标点转换
                    cand.start or 0, -- 分词开始位置
                    cand._end or 0, -- 分词结束位置  
                    new_text, -- 替换后的文本
                    cand_comment or "" -- 保持原有注释
                )
                -- 保持其他重要属性
                if cand.preedit then
                    new_cand.preedit = cand.preedit
                end
                yield(new_cand) -- 输出新的候选词
            else

                -- 如果没有文本或不包含标点符号，将comment中的chinese_pos去掉
                if cand.comment and cand.comment:match("^chinese_pos:") then
                    -- logger.info("候选词为chinese_pos, 删除comment, 格式为chinese_pos:1,2,9,10,")
                    cand.comment = cand.comment:gsub("^chinese_pos:[%d,]+", "")
                end

                if cand_type == "web_cloud" then
                    cand.comment = "   [云输入]"
                elseif cand_type == "ai_cloud" then
                    cand.comment = "   [AI识别]"
                elseif cand_type:sub(1, #"ai_cloud") == "ai_cloud" then
                    local suffix = cand_type:match("([^/]+)$")
                    cand.comment = "   [" .. suffix .. "]"
                elseif cand_type == "rawenglish_combo" then
                    cand.comment = ""
                end
                yield(cand)
            end
        end

        logger.info("标点符号过滤器处理完成")
    end)

    -- 处理错误情况
    if not success then
        local error_message = tostring(error_msg)
        logger.error("标点符号过滤器发生错误: " .. error_message)

        -- 记录详细的错误信息用于调试
        logger.error("错误堆栈信息: " .. debug.traceback())

        -- 在发生错误时,安全地输出原始候选词
        for cand in translation:iter() do
            yield(cand)
        end
    end
end

function punct_eng_chinese_filter.fini(env)
    logger.info("punct_eng_chinese_filter结束运行")
end

return punct_eng_chinese_filter
