-- lua/spans_manager.lua
-- Spans 信息统一管理模块
-- 用于管理候选词的分割信息，支持光标跳转功能

local logger_module = require("logger")

-- 创建日志记录器
local logger = logger_module.create("spans_manager", {
    enabled = true,
    unique_file_log = false, -- 启用日志以便测试
    log_level = "DEBUG"
})

local spans_manager = {}

-- 常量定义
local SPANS_VERTICES_KEY = "spans_vertices"
local SPANS_INPUT_KEY = "spans_input" 
local SPANS_SOURCE_KEY = "spans_source"
local SPANS_TIMESTAMP_KEY = "spans_timestamp"

-- 来源优先级（数字越小优先级越高）
local SOURCE_PRIORITY = {
    rawenglish_translator = 1,
    cloud_ai_filter_v2 = 2,
    baidu_filter = 2,
    punct_eng_chinese_filter = 3,
    unknown = 99
}

-- 统一保存 spans 信息
-- @param context: 上下文对象
-- @param vertices: 分割点数组
-- @param input: 对应的输入内容
-- @param source: 来源脚本名称
function spans_manager.save_spans(context, vertices, input, source)
    if not context or not vertices or not input then
        logger.error("save_spans: 参数不能为空")
        return false
    end
    
    source = source or "unknown"
    
    -- 检查是否已存在 spans 信息
    local existing_source = context:get_property(SPANS_SOURCE_KEY) or ""
    local existing_input = context:get_property(SPANS_INPUT_KEY) or ""
    
    -- 如果已存在且来源优先级更高，则不覆盖
    if existing_source ~= "" then
        local existing_priority = SOURCE_PRIORITY[existing_source] or 99
        local new_priority = SOURCE_PRIORITY[source] or 99
        
        if new_priority > existing_priority then
            logger.info(string.format("save_spans: 跳过保存，已有更高优先级的spans (现有:%s[%d] vs 新:%s[%d])", 
                existing_source, existing_priority, source, new_priority))
            return false
        end
        
        -- 如果输入内容相同且优先级相同，也跳过
        if existing_input == input and new_priority == existing_priority then
            logger.debug("save_spans: 跳过保存，输入内容和优先级相同")
            return false
        end
    end
    
    -- 将 vertices 转换为字符串格式
    local vertices_str = ""
    for i, vertex in ipairs(vertices) do
        vertices_str = vertices_str .. tostring(vertex)
        if i < #vertices then
            vertices_str = vertices_str .. ","
        end
    end
    
    -- 保存 spans 相关信息
    context:set_property(SPANS_VERTICES_KEY, vertices_str)
    context:set_property(SPANS_INPUT_KEY, input)
    context:set_property(SPANS_SOURCE_KEY, source)
    context:set_property(SPANS_TIMESTAMP_KEY, tostring(os.time()))
    
    logger.info(string.format("save_spans: 保存成功 [来源:%s] [输入:%s] [分割点:%s]", 
        source, input, vertices_str))
    
    return true
end

-- 获取 spans 信息
-- @param context: 上下文对象
-- @return: {vertices_str, input, source, timestamp} 或 nil
function spans_manager.get_spans(context)
    -- if not context then
    --     logger.error("get_spans: context 不能为空")
    --     return nil
    -- end
    
    local vertices_str = context:get_property(SPANS_VERTICES_KEY) or ""
    local input = context:get_property(SPANS_INPUT_KEY) or ""
    local source = context:get_property(SPANS_SOURCE_KEY) or ""
    local timestamp = context:get_property(SPANS_TIMESTAMP_KEY) or ""
    
    if vertices_str == "" or input == "" then
        logger.debug("vertices_str == 空")
        return nil
    end
    
    return {
        vertices_str = vertices_str,
        input = input,
        source = source,
        timestamp = timestamp,
        vertices = spans_manager.parse_vertices_string(vertices_str)
    }
end

-- 解析 vertices 字符串为数组
-- @param vertices_str: 逗号分隔的字符串
-- @return: 数字数组
function spans_manager.parse_vertices_string(vertices_str)
    if not vertices_str or vertices_str == "" then
        return {}
    end
    
    local vertices = {}
    for vertex_str in vertices_str:gmatch("[^,]+") do
        local vertex = tonumber(vertex_str)
        if vertex then
            table.insert(vertices, vertex)
        end
    end
    
    return vertices
end

-- 清除 spans 信息
-- @param context: 上下文对象
-- @param reason: 清除原因
function spans_manager.clear_spans(context, reason)
    if not context then
        logger.error("clear_spans: context 不能为空")
        return
    end
    
    reason = reason or "未指定原因"
    
    local existing_spans = spans_manager.get_spans(context)
    if existing_spans then
        logger.info(string.format("clear_spans: 清除spans信息 [原因:%s] [原输入:%s] [原来源:%s]", 
            reason, existing_spans.input, existing_spans.source))
    end
    
    context:set_property(SPANS_VERTICES_KEY, "")
    context:set_property(SPANS_INPUT_KEY, "")
    context:set_property(SPANS_SOURCE_KEY, "")
    context:set_property(SPANS_TIMESTAMP_KEY, "")
end

-- 判断是否应该清除 spans 信息
-- @param context: 上下文对象
-- @param current_input: 当前输入内容
-- @return: {should_clear, reason}
function spans_manager.should_clear(context, current_input)
    if not context then
        return true, "context为空"
    end
    
    local existing_spans = spans_manager.get_spans(context)
    if not existing_spans then
        return false, "无spans信息"
    end
    
    current_input = current_input or context.input or ""
    
    -- 输入内容发生变化
    if current_input ~= existing_spans.input then
        return true, "输入内容变化"
    end
    
    -- -- 不再包含反引号（如果原来包含的话）
    -- if existing_spans.input:find("`") and not current_input:find("`") then
    --     return true, "不再包含反引号"
    -- end
    
    -- 组合状态结束
    if not context:is_composing() then
        return true, "组合状态结束"
    end
    
    -- -- spans 信息过期（超过30秒）
    -- local current_time = os.time()
    -- local spans_time = tonumber(existing_spans.timestamp) or 0
    -- if current_time - spans_time > 30 then
    --     return true, "spans信息过期"
    -- end
    
    return false, "无需清除"
end

-- 自动清除检查（在各个脚本中调用）
-- @param context: 上下文对象
-- @param current_input: 当前输入内容
-- @return: 是否执行了清除操作
function spans_manager.auto_clear_check(context, current_input)
    local should_clear, reason = spans_manager.should_clear(context, current_input)
    if should_clear then
        spans_manager.clear_spans(context, reason)
        return true
    end
    return false
end

-- 从候选词中提取并保存 spans 信息
-- @param context: 上下文对象
-- @param candidate: 候选词对象
-- @param input: 输入内容
-- @param source: 来源脚本
-- @return: 是否成功保存
function spans_manager.extract_and_save_from_candidate(context, candidate, input, source)
    if not candidate then
        logger.error("extract_and_save_from_candidate: candidate 不能为空")
        return false
    end
    
    local success, spans = pcall(function()
        return candidate:spans()  -- 这里不会返回最外面函数,而是返回pcall函数外面
    end)
    
    if not success or not spans then
        logger.debug("extract_and_save_from_candidate: 候选词无spans信息")
        return false
    end
    
    logger.debug("extract_and_save_from_candidate: 候选词包含spans信息，继续处理")
    local vertices = spans.vertices
    if not vertices or #vertices == 0 then
        logger.debug("extract_and_save_from_candidate: spans中无vertices信息")
        return false
    end
    logger.info("extract_and_save_from_candidate函数中执行save_spans")
    return spans_manager.save_spans(context, vertices, input, source)
end

-- 获取用于光标跳转的下一个位置
-- @param context: 上下文对象
-- @param current_pos: 当前光标位置
-- @return: 下一个位置，如果没有则返回nil
function spans_manager.get_next_cursor_position(context, current_pos)
    local spans_info = spans_manager.get_spans(context)
    if not spans_info then
        return nil
    end
    
    local vertices = spans_info.vertices
    local input_length = #(context.input or "")
    
    -- 如果光标在末尾，跳转到开头（第一个分割点通常是0）
    if current_pos >= input_length then
        if #vertices >= 2 then
            return vertices[2] -- 返回第二个分割点（第一个通常是0）
        end
        return 0
    end
    
    -- 查找下一个分割点
    for i, vertex in ipairs(vertices) do
        if vertex > current_pos then
            return vertex
        end
    end
    
    -- 如果没有找到更大的分割点，跳转到末尾
    return input_length
end

-- 获取用于光标跳转的上一个位置
-- @param context: 上下文对象  
-- @param current_pos: 当前光标位置
-- @return: 上一个位置，如果没有则返回nil
function spans_manager.get_prev_cursor_position(context, current_pos)
    local spans_info = spans_manager.get_spans(context)
    if not spans_info then
        return nil
    end
    
    local vertices = spans_info.vertices
    local input_length = #(context.input or "")
    
    -- 如果光标在开头，跳转到末尾
    if current_pos <= 0 then
        return input_length
    end
    
    -- 从后往前查找上一个分割点
    for i = #vertices, 1, -1 do
        if vertices[i] < current_pos then
            return vertices[i]
        end
    end
    
    -- 如果没有找到更小的分割点，跳转到开头
    return 0
end

-- 调试信息输出
-- @param context: 上下文对象
function spans_manager.debug_info(context)
    local spans_info = spans_manager.get_spans(context)
    if spans_info then
        logger.info("=== Spans Debug Info ===")
        logger.info("输入: " .. spans_info.input)
        logger.info("来源: " .. spans_info.source)
        logger.info("时间戳: " .. spans_info.timestamp)
        logger.info("分割点: " .. spans_info.vertices_str)
        logger.info("分割点数组: " .. table.concat(spans_info.vertices, ","))
        logger.info("========================")
    else
        logger.info("=== Spans Debug Info ===")
        logger.info("无spans信息")
        logger.info("========================")
    end
end

return spans_manager
