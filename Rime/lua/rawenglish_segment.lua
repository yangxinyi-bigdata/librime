-- 整个这段是非常简单的,就是判断如果说last_segment中含有反引号, 就将整个segmentation延伸到最后,全部标记上rawenglish标签
-- 然后由translator当中的lua/script_rawenglish_translator.lua 处理
local logger_module = require("logger")
local debug_utils = require("debug_utils")

-- 创建当前模块的日志记录器
local logger = logger_module.create("rawenglish_segment", {
    enabled = true, -- 启用日志以便测试
    unique_file_log = false, -- 启用日志以便测试
    log_level = "DEBUG"
})

-- 初始化时清空日志文件
logger.clear()

local segmentor = {}
segmentor.english_mode_symbol = "`" -- 默认值

-- 配置更新函数
function segmentor.update_current_config(config)
    logger.debug("开始更新rawenglish_segment模块配置")

    segmentor.english_mode_symbol = config:get_string("translator/english_mode_symbol") or "`"
    logger.debug("英文模式符号: " .. tostring(segmentor.english_mode_symbol))

    logger.debug("rawenglish_segment模块配置更新完成")
end

function segmentor.init(env)
    -- 配置更新由 cloud_input_processor 统一管理，无需在此处调用
    local config = env.engine.schema.config
    logger.debug("等待 cloud_input_processor 统一更新配置")

    logger.debug("rawenglish_segment初始化完成")
    logger.debug("=" .. string.rep("=", 60))
end

function segmentor.func(segmentation, env)
    local context = env.engine.context
    local input = segmentation.input
    local english_mode_symbol = segmentor.english_mode_symbol
    logger.debug("")

    -- logger.debug("刚进入时的segmentation:")
    -- debug_utils.print_segmentation_info(segmentation, logger)

    local current_start = segmentation:get_current_start_position()
    local current_end = segmentation:get_current_end_position()
    local current_start_input = input:sub(current_start + 1)
    logger.debug("current_start_input: " .. current_start_input)

    -- 检测以反引号片段开头的情况
    if #current_start_input > 1 and current_start_input:sub(1, 1) == english_mode_symbol then
        -- 查找第一个反引号片段的结束位置
        local rawenglish_end = current_start_input:find(english_mode_symbol, 2)
        local rawenglish_length, rawenglish_content

        if not rawenglish_end then
            -- 没有找到配对的结束反引号，将整个输入作为反引号片段
            rawenglish_length = #current_start_input
            rawenglish_content = current_start_input
            logger.debug("检测到未闭合的反引号片段:")
            if context:get_property("rawenglish_prompt") == "0" then
                logger.debug("rawenglish_prompt提示标志为 0, 设置为 1")
                context:set_property("rawenglish_prompt", "1")
            end

            logger.debug("  反引号片段: '" .. rawenglish_content .. "' (长度: " .. rawenglish_length .. ")")

            -- 添加反引号片段的segment
            local rawenglish_segment = Segment(current_start, current_start + rawenglish_length)
            rawenglish_segment.tags = Set {"single_rawenglish"}

            -- segmentation:forward()
            if segmentation:add_segment(rawenglish_segment) then
                logger.debug("成功添加反引号片段segment (start: " .. current_start .. ", end: " ..
                                (current_start + rawenglish_length) .. ")")
                segmentation:forward()
                return false -- 完成分词, 因为整段都分词完成了

            else
                logger.error("无法添加反引号片段segment")
            end
        else
            -- 找到配对的结束反引号
            rawenglish_length = rawenglish_end
            rawenglish_content = current_start_input:sub(1, rawenglish_length)
            logger.debug("检测到完整的反引号片段:")
            if context:get_property("rawenglish_prompt") == "1" then
                logger.debug("rawenglish_prompt提示标志为 1 , 设置为 0")
                context:set_property("rawenglish_prompt", "0")
            end

            logger.debug("  反引号片段: '" .. rawenglish_content .. "' (长度: " .. rawenglish_length .. ")")

            -- 添加反引号片段的segment
            local rawenglish_segment = Segment(current_start, current_start + rawenglish_length)
            rawenglish_segment.tags = Set {"single_rawenglish"}

            -- segmentation:forward()
            if segmentation:add_segment(rawenglish_segment) then
                logger.debug("成功添加反引号片段segment (start: " .. current_start .. ", end: " ..
                                (current_start + rawenglish_length) .. ")")

                local current_end = segmentation:get_current_end_position()
                if current_end == #segmentation.input then
                    -- 分词完成了
                    return false
                else
                    -- 继续分词, 下面还有代码继续分词
                    segmentation:forward()
                end

            else
                logger.error("无法添加反引号片段segment")
            end

        end

    end

    logger.debug("处理完第一段之后的segmentation:")
    debug_utils.print_segmentation_info(segmentation, logger)

    local current_start = segmentation:get_current_start_position()
    local current_end = segmentation:get_current_end_position()
    local current_start_input = input:sub(current_start + 1)
    logger.debug("current_start_input: " .. current_start_input)

    local _, rawenglish_count = current_start_input:gsub(english_mode_symbol, "")
    if rawenglish_count % 2 == 1 then
        logger.debug("检测到奇数个反引号,存在未闭合情况: " .. current_start_input ..
                         " (反引号数量: " .. rawenglish_count .. ")")
        -- 只在值真正需要改变时才设置
        -- 先获取当前选项的值，避免不必要的更新
        logger.debug("当前英文模式rawenglish_prompt: " .. context:get_property("rawenglish_prompt"))
        if context:get_property("rawenglish_prompt") == "0" then
            logger.debug("rawenglish_prompt提示标志为 0, 设置为 1")
            context:set_property("rawenglish_prompt", "1")
            logger.debug("rawenglish_prompt 已设置为 1")
        end

        -- 将最后整段都标记成Set {"rawenglish_combo", "abc"}

    else
        logger.debug(
            "检测到偶数个反引号: " .. current_start_input .. " (反引号数量: " .. rawenglish_count .. ")")
        -- 如果不在组词状态或没有达到触发条件,则重置提示选项
        logger.debug("当前不在反引号当中rawenglish提示已重置")
        if context:get_property("rawenglish_prompt") == "1" then
            context:set_property("rawenglish_prompt", "0")
            logger.debug("rawenglish_prompt 已设置为 0")
        end
    end

    if current_start_input:find(english_mode_symbol) then
        logger.debug("当前段存在反引号")
        local new_segment = Segment(current_start, #segmentation.input)
        new_segment.tags = Set {"rawenglish_combo", "abc"}
        if segmentation:add_segment(new_segment) then
            logger.debug("成功将最后一个segment延长到末尾")
            return false
        else
            logger.error("无法将最后一个segment延长到末尾")
            return true
        end
    end

    logger.debug("")
    logger.debug("=" .. string.rep("=", 60))

    -- 返回true继续处理，false停止处理
    logger.debug("return true")
    debug_utils.print_segmentation_info(segmentation, logger)
    return true
end

function segmentor.fini(env)
    logger.debug("调试分词器结束")
end

return segmentor
