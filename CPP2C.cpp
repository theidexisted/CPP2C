#include <clang/AST/AST.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Driver/Options.h>
#include <clang/Frontend/ASTConsumers.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>
#include <iostream>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/Debug.h>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <vector>

#define DEBUG_TYPE "promethus_gen"

using namespace std;
using namespace clang;
using namespace clang::driver;
using namespace clang::tooling;
using namespace clang::ast_matchers;
using namespace llvm;

/** Options **/
static cl::OptionCategory CPP2CCategory("CPP2C options");
static cl::opt<std::string> OutputFilename(
    "o", cl::desc(getDriverOptTable().getOptionHelpText((options::OPT_o))),
    cl::cat(CPP2CCategory));

static cl::opt<bool, true> Debug("debug", cl::desc("Enable debug output"),
                                 cl::location(DebugFlag),
                                 cl::cat(CPP2CCategory));

static cl::opt<std::string> ClassesToGenrate(
    "classes", cl::desc("Classes to generate C binding, split by space"),
    cl::init("uThread kThread Cluster Connection Mutex OwnerLock "
             "ConditionVariable Semaphore uThreadPool"),
    cl::cat(CPP2CCategory));

llvm::SmallVector<llvm::StringRef, 16> ClassList;
map<string, int> funcList;

class Gen {
  void Run() {
    std::string fileTemplate = R"(
class Guage {
public:
  enum class GuageType {
## for e in GuageEnums 
      {{ e }},
## endfor 
  };
  const char* GetName(GuageType type);
  const std::vector<const char*> GetLabels(GuageType type);
private:
  static const size_t kEnumNum = {{ GuageEnumNum }};
  std::atomic_int_fast64_t data_[kEnumNum];
};

class Counter {
pubilc:
   enum class CounterType {
## for e in CounterEnums 
      {{ e }},
## endfor 
  };
  const char* GetName(GuageType type);
  const std::vector<const char*> GetLabels(GuageType type);
private:
  static const size_t kEnumNum = {{ CounterEnumNum }};
  std::atomic_uint_fast64_t data_[kEnumNum];
};

class HistogramStat {
public:
  static const size_t BucketPoint[] = {
    1, 10, 50, 100, 500, 1000, 2000, 4000, 6000, 8000, 10000, 5e4, 1e5, 5e5, 1e6, 1e7, 1e8
  };
  constexpr std::size_t kBucketNum = sizeof(BucketPoint) / sizeof(BucketPoint[0]);
  std::array<std::atomic_uint_fast64_t, kBucketNum + 1> histogram_;
};

class Histogram {
pubilc:
   enum class HistogramType {
## for e in HistogramEnums 
      {{ e }},
## endfor 
  };
  const char* GetName(GuageType type);
  const std::vector<const char*> GetLabels(GuageType type);
private:
  static const size_t kEnumNum = {{ HistogramEnumNum }};
  HistogramStat stat[kEnumNum];
} 

class Statistics {
public:
  struct ALIGNAS(CACHE_LINE_SIZE) StatisticsData {
    Guage guage;
    Counter counter;
    Histogram histogram;
    char padding_data[CACHE_LINE_SIZE -
      ((sizeof(guage) + sizeof(counter) + sizeof(histogram)) % CACHE_LINE_SIZE)]

    void *operator new(size_t s) { return cacheline_aligned_alloc(s); }
    void *operator new[](size_t s) { return cacheline_aligned_alloc(s); }
    void operator delete(void *p) { cacheline_aligned_free(p); }
    void operator delete[](void *p) { cacheline_aligned_free(p); }
  };
  static_assert(sizeof(StatisticsData) % CACHE_LINE_SIZE == 0, "Expected " TOSTRING(CACHE_LINE_SIZE) "-byte aligned");

  void SetGuage(Guage::GuageType type, int64_t value);
  void AddGuage(Guage::GuageType type, int64_t value);
  void RestGuage(Guage::GuageType type, int64_t value);

  void AddCounter(Couter::CounterType type, int64_t value);
  void RestCounter(Couter::CounterType type, int64_t value);

  void RecordInHIstogram(Histogram::HistogramType type, int64_t value);

  void Reset();

private:
  int64_t GetGuageLocked(Guage::GuageType type) const;
  void SetGuageLocked(Guage::GuageType type, int64_t value);

  uint64_t GetCounterLocked(Couter::CounterType type);
  void SetCounterLocked(Couter::CounterType type, int64_t value);

  CoreLocalArray<StatisticsData> per_core_stats_;
  MicroSpinLock spin_lock_;
};

)";
  };
};

class Generator {
  public:
    virtual void AddInclude() = 0;
    virtual llvm::StringRef ClassName() = 0;
    virtual std::string Template() = 0;
    virtual void Run() = 0;
    virtual size_t EnumNum() = 0;
};

class GuageGenerator : public Generator {
  public:
    virtual void AddInclude() override {
    }
    virtual std::string Template() override {
      return R"(
class Guage {
public:
  enum class GuageType {
## for e in Enums 
      {{ e }},
## endfor 
  };
  void Add(GuageType type, int64_t value) {
    GetValueRef(type) += value;
  }
  int64_t& GetValueRef(GuageType type) {
    return data_[GetCoreIndex()][type];
  }
private:
  static const size_t kEnumNum = {{ EnumNum }};
  std::atomic_uint_fast64_t data_[MAXCORENUM][kEnumNum];
};
      )";
    }
/*
    virtual llvm::StringRef ClassName() override {
      return "Guage";
    }
    */
    virtual void Run() override {
    }
};

class CounterGenerator : public GuageGenerator {
    virtual llvm::StringRef ClassName() override {
      return "Counter";
    }
    virtual void Run() override {
    }
};

class HistogramGenerator : public Generator {
};

class HeaderWritter {
  public:
    std::string CommonHeader() {
      return R"(
#pragma once
#include <cstdint>
#include <atomic>

#include "core_local.h"
#include "port_posix.h"
#include "MicroSpinLock.h"

#define MAXCORENUM 64
#ifndef STRINGIFY
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#endif


size_t GetCoreIndex() {
  return 0;
}
      )";
    }
};

class EnumMatchHandler : public MatchFinder::MatchCallback {
public:
  EnumMatchHandler() = default;

  struct Property {
    std::string name;
    std::vector<std::string> labels;
    std::string ToString() {
      return "name:" + name +
             ",labels:" + llvm::join(labels.begin(), labels.end(), ",");
    }
  };

  bool ParseAnnotation(const Attr *attr, Property &prop) {
    llvm::SmallString<64> str;
    llvm::raw_svector_ostream os(str);
    LangOptions langopts;
    PrintingPolicy policy(langopts);
    attr->printPretty(os, policy);

    auto left = str.find('\"');
    auto right = str.rfind('\"');
    if (left == llvm::StringRef::npos || right == llvm::StringRef::npos ||
        left >= right) {
      LLVM_DEBUG(
          llvm::dbgs()
          << "The attr content parse failed, no \" founded or no content\n");
      return false;
    }
    llvm::StringRef content = str.slice(left + 1, right);

    llvm::SmallVector<llvm::StringRef, 4> valuePair;
    llvm::SplitString(content, valuePair, ":");

    assert(!valuePair.empty());

    if (valuePair[0] == "name") {
      prop.name = valuePair[1];
    } else if (valuePair[0] == "labels") {
      llvm::SmallVector<llvm::StringRef, 4> labels;
      llvm::SplitString(valuePair[1], labels, ",");
      prop.labels = {labels.begin(), labels.end()};
    } else {
      LLVM_DEBUG(llvm::dbgs()
                 << "Can't parse the content, no name or labels pairs found\n");
      return false;
    }

    return true;
  }
  virtual void run(const MatchFinder::MatchResult &Result) {
    if (const EnumDecl *cmd = Result.Nodes.getNodeAs<EnumDecl>("enum")) {
      LLVM_DEBUG(llvm::dbgs() << "Match an enum:"
                              << cmd->getQualifiedNameAsString() << '\n');
      for (const auto &enumConstant : cmd->enumerators()) {
        Property prop;
        for (const auto &attr : enumConstant->attrs()) {
          if (attr->getKind() == attr::Annotate) {
            ParseAnnotation(attr, prop);
          }
        }
        LLVM_DEBUG(llvm::dbgs()
                   << "For enum:" << enumConstant->getQualifiedNameAsString()
                   << ' ' << "label parsed result:\n" << prop.ToString() << '\n');
      }
    }
  }
};

class EnumASTConsumer : public ASTConsumer {
public:
  EnumASTConsumer() {
    DeclarationMatcher enumMatcher = enumDecl(isDefinition()).bind("enum");
    Matcher.addMatcher(enumMatcher, &handler);
  }

  void HandleTranslationUnit(ASTContext &Context) override {
    Matcher.matchAST(Context);
  }

private:
  EnumMatchHandler handler;
  MatchFinder Matcher;
};

class MyFrontendAction : public ASTFrontendAction {
public:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef file) override {

    return std::make_unique<EnumASTConsumer>();
  }
};

int main(int argc, const char **argv) {
  // parse the command-line args passed to your code
  CommonOptionsParser op(argc, argv, CPP2CCategory);
  // create a new Clang Tool instance (a LibTooling environment)
  ClangTool Tool(op.getCompilations(), op.getSourcePathList());

  llvm::SplitString(ClassesToGenrate, ClassList, " ");
  // run the Clang Tool, creating a new FrontendAction
  return Tool.run(newFrontendActionFactory<MyFrontendAction>().get());
}
