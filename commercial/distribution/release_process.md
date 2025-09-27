# 版本发布流程

## 🚀 发布流程概览

### 阶段1: 开发完成
```bash
# 1. 代码完成并测试
# 2. 更新版本号
# 3. 生成变更日志
```

### 阶段2: 商业发布准备
```bash
# 1. 编译发布版本
./scripts/build_commercial.sh 5.1

# 2. 生成许可证模板
./scripts/generate_license.sh 5.1

# 3. 创建分发包
./scripts/package_release.sh 5.1
```

### 阶段3: 商业分发
```bash
# 1. 上传到分发平台
# 2. 配置支付系统
# 3. 启动营销活动
```

### 阶段4: 开源转换 (2个版本后)
```bash
# 1. 移动源代码到公开目录
./scripts/convert_to_opensource.sh 5.1

# 2. 更新许可证为MIT
# 3. 提交到公开仓库
git add . && git commit -m "Release version 5.1 as open source"
```

## 📋 发布检查清单

### 开发阶段检查
- [ ] 所有功能开发完成
- [ ] 单元测试通过
- [ ] 集成测试通过
- [ ] 性能测试达标
- [ ] 安全审计完成
- [ ] 文档更新完成

### 商业发布检查
- [ ] 版本号更新
- [ ] 变更日志编写
- [ ] 许可证生成
- [ ] 发布包创建
- [ ] 数字签名验证
- [ ] 分发渠道配置

### 质量保证检查
- [ ] 安装测试
- [ ] 功能验证
- [ ] 兼容性测试
- [ ] 用户体验测试
- [ ] 客户支持准备

### 营销准备检查
- [ ] 产品介绍更新
- [ ] 定价策略确认
- [ ] 促销活动准备
- [ ] 社区公告准备
- [ ] 技术支持培训

## 🔧 自动化脚本

### 构建脚本 (build_commercial.sh)
```bash
#!/bin/bash
VERSION=$1
echo "Building commercial version $VERSION..."

# 编译源代码
make clean
make release VERSION=$VERSION

# 混淆关键代码
./tools/obfuscate.sh src/

# 生成发布包
mkdir -p commercial/versions/$VERSION/release
cp -r build/* commercial/versions/$VERSION/release/

echo "Build completed for version $VERSION"
```

### 许可证生成脚本 (generate_license.sh)
```bash
#!/bin/bash
VERSION=$1
TEMPLATE="commercial/licensing/commercial_license.txt"
OUTPUT="commercial/versions/$VERSION/license_template.txt"

# 替换版本号
sed "s/{VERSION}/$VERSION/g" $TEMPLATE > $OUTPUT

echo "License template generated for version $VERSION"
```

### 开源转换脚本 (convert_to_opensource.sh)
```bash
#!/bin/bash
VERSION=$1
SOURCE_DIR="commercial/versions/$VERSION/source"
TARGET_DIR="$VERSION"

echo "Converting version $VERSION to open source..."

# 移动源代码
cp -r $SOURCE_DIR/* $TARGET_DIR/

# 更新许可证头部
find $TARGET_DIR -name "*.cpp" -o -name "*.h" | xargs ./scripts/update_license_headers.sh

# 创建开源发布包
cd $TARGET_DIR
zip -r "../release/慕容调度$VERSION.zip" .
cd ..

echo "Version $VERSION converted to open source"
```

## 📊 发布指标跟踪

### 销售指标
- 预售数量
- 正式销售数量
- 收入统计
- 退款率

### 技术指标
- 下载次数
- 安装成功率
- 使用活跃度
- 错误报告数量

### 用户反馈
- 满意度评分
- 功能请求
- 问题报告
- 改进建议

## 🎯 版本生命周期

```
开发阶段 (2-3个月)
    ↓
内测阶段 (2周)
    ↓
商业发布 (付费销售)
    ↓
维护更新 (持续)
    ↓
开源转换 (2个版本后)
    ↓
社区维护 (长期)
```

## 📞 紧急联系方式

- **技术负责人**: 慕容茹艳
- **QQ群**: 974835379
- **邮箱**: support@muronggameopt.com
- **紧急热线**: 24小时技术支持

---

**注意**: 严格按照流程执行，确保每个步骤都有相应的验证和备份。