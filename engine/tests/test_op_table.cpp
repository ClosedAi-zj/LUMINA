// T0.3 验收：OpTable stage 校验（错序/未知算子运行期报错）+ 骨架算子 + log16 曝光语义
#include <cstdio>
#include <memory>
#include <string>

#include "plan/op_table.h"
#include "plan/ops/ops.h"

using namespace lumina;

namespace {

int gFailures = 0;

void check(bool condition, const std::string& name) {
  if (condition) {
    printf("  [PASS] %s\n", name.c_str());
  } else {
    printf("  [FAIL] %s\n", name.c_str());
    ++gFailures;
  }
}

// 测试用可注入曝光算子（与 ops/basic.cpp 的 ExposureOp 同数学：log16 域整数加法）
class TestExposureOp : public plan::Operator {
 public:
  TestExposureOp() { (void)table_; }
  const char* name() const override { return "test.exposure"; }
  plan::Stage stage() const override { return plan::Stage::S2Tone; }
  bool apply(imaging::ImageBuf* buf, std::string* err) const override {
    if (!buf || !buf->valid()) {
      if (err) *err = "test.exposure: invalid buffer";
      return false;
    }
    double delta = 1.0 * buf->logStep;  // +1EV = 2x 增益
    for (size_t i = 0; i < buf->data.size(); ++i) {
      double c = static_cast<double>(buf->data[i]) + delta;
      buf->data[i] = static_cast<uint16_t>(c > 65535.0 ? 65535.0 : c + 0.5);
    }
    return true;
  }

 private:
  plan::OpTable* table_ = nullptr;
};

}  // namespace

int main() {
  printf("== test_op_table ==\n");
  std::string err;
  plan::OpTable& table = plan::OpTable::instance();

  // 用例 1：内置骨架注册（8 节点 + output，≥ 20 个算子名），重名拒绝
  check(plan::registerBuiltinOps(&table, &err), "case1: registerBuiltinOps 成功");
  check(table.size() >= 20, "case1b: 注册算子数 >= 20（实测 " + std::to_string(table.size()) + "）");
  int before = static_cast<int>(table.size());
  std::shared_ptr<plan::Operator> dup(
      new plan::IdentityOp("basic.exposure", plan::Stage::S2Tone));
  check(!table.registerOp(dup, &err) && err.find("duplicate") != std::string::npos,
        "case1c: 重名注册被拒");

  // 用例 2：合法 stage 顺序求值（S0 → S2 → S4 → S7）
  imaging::ImageBuf buf(4, 4);
  for (size_t i = 0; i < buf.data.size(); ++i) buf.data[i] = 24576;  // 1.0
  check(table.evaluate({"basic.whiteBalance", "basic.exposure", "curve.toneCurveRGB",
                        "output.encode"},
                       &buf, &err),
        "case2: 合法顺序求值通过 " + err);

  // 用例 3：stage 逆序 → 运行期报错（S4 之后再跑 S2 算子）
  imaging::ImageBuf buf2(4, 4);
  bool rejected = !table.evaluate({"curve.toneCurveRGB", "basic.exposure"}, &buf2, &err);
  check(rejected, "case3a: stage 逆序被拒");
  check(err.find("stage order violation") != std::string::npos, "case3b: 报错含 stage order violation");

  // 用例 4：未知算子 → 报错
  imaging::ImageBuf buf3(4, 4);
  check(!table.evaluate({"no.such.op"}, &buf3, &err) &&
            err.find("unknown operator") != std::string::npos,
        "case4: 未知算子报错");

  // 用例 5：无效缓冲 → 报错
  imaging::ImageBuf bad;
  check(!table.evaluate({"basic.exposure"}, &bad, &err), "case5: invalid buffer 报错");

  // 用例 6：+1EV 曝光 = log16 域精确 +2048 码（2x 线性增益）
  {
    plan::OpTable& t2 = plan::OpTable::instance();
    t2.registerOp(std::make_shared<TestExposureOp>(), &err);
    imaging::ImageBuf b(2, 2);
    b.data[0] = imaging::log16Encode(0.25f);  // -2EV
    b.data[1] = 24576;                        // 1.0
    check(t2.evaluate({"test.exposure"}, &b, &err), "case6a: 求值通过");
    double v0 = imaging::log16Decode(b.data[0]);
    double v1 = imaging::log16Decode(b.data[1]);
    check(v0 > 0.499 && v0 < 0.501, "case6b: 0.25 → 0.5（精确 2x）");
    check(v1 > 1.999 && v1 < 2.001, "case6c: 1.0 → 2.0（精确 2x）");
  }

  // 用例 7：同 stage 可重复，stage 单调推进后再回到旧 stage 被拒
  imaging::ImageBuf buf4(4, 4);
  check(table.evaluate({"curve.toneCurveRGB", "curve.toneCurveR"}, &buf4, &err),
        "case7a: 同 stage 重复允许");
  imaging::ImageBuf buf5(4, 4);
  check(!table.evaluate({"detail.sharpen", "curve.toneCurveRGB"}, &buf5, &err),
        "case7b: S5 之后再跑 S4 被拒");

  (void)before;
  printf("== test_op_table %s ==\n", gFailures == 0 ? "ALL PASS" : "FAILED");
  return gFailures == 0 ? 0 : 1;
}
