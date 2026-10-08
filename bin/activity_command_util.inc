/*
 * 通用命令执行/字符串工具。
 *
 * 原先与已删除的授权校验模块放在同一个文件里；拆分后继续提供给场景识别
 * （activity_scene_policy.inc）与配置解析共用。
 */
static bool execute_command_trim(const char* cmd, char* out, size_t out_size) {
    if (!cmd || !out || out_size == 0) return false;
    out[0] = '\0';
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return false;
    bool ok = false;
    if (fgets(out, (int)out_size, pipe)) {
        out[strcspn(out, "\r\n")] = '\0';
        ok = out[0] != '\0';
    }
    pclose(pipe);
    return ok;
}

static bool execute_command_all(const char* cmd, std::string& out) {
    out.clear();
    if (!cmd) return false;
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return false;

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        out += buffer;
    }
    pclose(pipe);
    return !out.empty();
}

static std::string trim_copy(const std::string& text) {
    size_t start = 0;
    while (start < text.size() && isspace((unsigned char)text[start])) start++;
    size_t end = text.size();
    while (end > start && isspace((unsigned char)text[end - 1])) end--;
    return text.substr(start, end - start);
}
