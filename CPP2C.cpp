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
#include <llvm/ADT/StringExtras.h>

#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <vector>

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

static cl::opt<std::string> ClassesToGenrate(
    "classes", cl::desc("Classes to generate C binding, split by space"),
    cl::init("uThread kThread Cluster Connection Mutex OwnerLock "
             "ConditionVariable Semaphore uThreadPool"),
    cl::cat(CPP2CCategory));

/** Classes to be mapped to C **/
struct OutputStreams {
  string headerString;
  string bodyString;

  llvm::raw_string_ostream HeaderOS;
  llvm::raw_string_ostream BodyOS;

  OutputStreams() : HeaderOS(headerString), BodyOS(bodyString){};
};

llvm::SmallVector<llvm::StringRef, 16> ClassList;
map<string, int> funcList;

/** Matchers **/

/** Handlers **/
class classMatchHandler : public MatchFinder::MatchCallback {
 public:
  classMatchHandler(OutputStreams &os) : OS(os) {}

  tuple<string, string, bool, bool> determineCType(const QualType &qt) {
    string CType;
    string CastType;  // whether this should be casted or not
    bool isPointer = false;
    bool shoulReturn = true;

    // if it is builtint type use it as is
    if (qt->isBuiltinType() ||
        (qt->isPointerType() && qt->getPointeeType()->isBuiltinType())) {
      CType = qt.getAsString();
      if (qt->isVoidType()) shoulReturn = false;
      // if it is a CXXrecordDecl then return a pointer to WName*
    } else if (qt->isRecordType()) {
      const CXXRecordDecl *crd = qt->getAsCXXRecordDecl();
      string recordName = crd->getNameAsString();
      CType = "W" + recordName + "*";
      CastType = recordName + "*";

    } else if ((qt->isReferenceType() || qt->isPointerType()) &&
               qt->getPointeeType()->isRecordType()) {
      isPointer = true;  // to properly differentiate among cast types
      const CXXRecordDecl *crd = qt->getPointeeType()->getAsCXXRecordDecl();
      string recordName = crd->getNameAsString();
      if (std::find(ClassList.begin(), ClassList.end(), recordName) !=
          ClassList.end()) {
        CType = "W" + recordName + "*";
        CastType = recordName + "*";
      } else {
        CType = recordName + "*";
      }
    }
    return make_tuple(CType, CastType, isPointer, shoulReturn);
  }

  virtual void run(const MatchFinder::MatchResult &Result) {
    if (const CXXMethodDecl *cmd =
            Result.Nodes.getNodeAs<CXXMethodDecl>("publicMethodDecl")) {
      string methodName;
      string className = cmd->getParent()->getDeclName().getAsString();
      string returnType;
      string returnCast;
      bool shouldReturn, isPointer;
      string self = "W" + className + "* self";
      string separator = ", ";
      string bodyEnd;

      std::stringstream functionBody;

      // ignore operator overloadings
      if (cmd->isOverloadedOperator()) return;

      std::string ret;

      auto isConst = cmd->isConst();

      if (cmd->isVirtual()) {
        auto methodName = cmd->getNameAsString();

        const QualType qt = cmd->getReturnType();
        std::tie(returnType, returnCast, isPointer, shouldReturn) =
            determineCType(qt);

        ret.append("MOCK_METHOD(");
        ret.append(methodName);
        ret.append(",");
        ret.append(returnType);
        ret.append("(");

        auto n = cmd->getNumParams();
        if (n) {
          for (unsigned int i = 0; i < n; i++) {
            ret.append(cmd->parameters()[i]->getQualifiedNameAsString());
            if (i != n - 1) ret.append(",");
          }
        }
        ret.append(")");

        std::cout << ret << std::endl;
      }

      /*
      // constructor
      if (const CXXConstructorDecl *ccd = dyn_cast<CXXConstructorDecl>(cmd)) {
        if (ccd->isCopyConstructor() || ccd->isMoveConstructor()) return;
        methodName = "_create";
        returnType = "W" + className + "*";
        self = "";
        separator = "";
        functionBody << "return reinterpret_cast<" << returnType << ">( new "
                     << className << "(";
        bodyEnd += "))";
      } else if (isa<CXXDestructorDecl>(cmd)) {
        methodName = "_destroy";
        returnType = "void";
        functionBody << " delete reinterpret_cast<" << className << "*>(self)";
      } else {
        methodName = "_" + cmd->getNameAsString();
        const QualType qt = cmd->getReturnType();
        std::tie(returnType, returnCast, isPointer, shouldReturn) =
            determineCType(qt);

        // should this function return?
        if (shouldReturn) functionBody << "return ";

        if (returnCast != "") {
          // if not pointer and it needs to be casted, then return the pointer
          if (!isPointer) functionBody << "&";
          functionBody << "reinterpret_cast<" << returnType << ">(";
          bodyEnd += ")";
        }

        // if Static call it properly
        if (cmd->isStatic())
          functionBody << className << "::" << cmd->getNameAsString() << "(";
        // if not  use the passed object to call the method
        else
          functionBody << "reinterpret_cast<" << className << "*>(self)->"
                       << cmd->getNameAsString() << "(";

        bodyEnd += ")";
      }

      std::stringstream funcname;
      funcname << returnType << " " << className << methodName;

      auto it = funcList.find(funcname.str());

      if (it != funcList.end()) {
        it->second++;
        funcname << "_" << it->second;
      } else {
        funcList[funcname.str()] = 0;
      }

      funcname << "(" << self;

      for (unsigned int i = 0; i < cmd->getnumparams(); i++) {
        const qualtype qt = cmd->parameters()[i]->gettype();
        std::tie(returntype, returncast, ispointer, shouldreturn) =
            determinectype(qt);
        funcname << separator << returntype << " ";
        funcname << cmd->parameters()[i]->getqualifiednameasstring() << "";

        if (i != 0) functionbody << separator;
        if (returncast == "")
          functionbody << cmd->parameters()[i]->getqualifiednameasstring();
        else {
          if (!ispointer) functionbody << "*";
          functionbody << "reinterpret_cast<" << returncast << ">("
                       << cmd->parameters()[i]->getqualifiednameasstring()
                       << ")";
        }

        string separator = ", ";
      }
      funcname << ")";

      OS.HeaderOS << funcname.str() << ";\n";

      OS.BodyOS << funcname.str() << "{\n    ";
      OS.BodyOS << functionBody.str();
      OS.BodyOS << bodyEnd << "; \n}\n";
      */
    }
  }
  virtual void onEndOfTranslationUnit() {}

 private:
  OutputStreams &OS;
};

/****************** /Member Functions *******************************/
// Implementation of the ASTConsumer interface for reading an AST produced
// by the Clang parser. It registers a couple of matchers and runs them on
// the AST.
class MyASTConsumer : public ASTConsumer {
 public:
  MyASTConsumer(OutputStreams &os) : OS(os), HandlerForClassMatcher(os) {
    // Add a simple matcher for finding 'if' statements.

    for (const auto &className : ClassList) {
      OS.HeaderOS << "struct      W" << className
                  << "; \n"
                     "typedef     struct W"
                  << className << " W" << className << ";\n";
      // oss.push_back(std::move(os))

      DeclarationMatcher classMatcher =
          cxxMethodDecl(isPublic(), ofClass(hasName(className)))
              .bind("publicMethodDecl");
      Matcher.addMatcher(classMatcher, &HandlerForClassMatcher);
    }
  }

  void HandleTranslationUnit(ASTContext &Context) override {
    // Run the matchers when we have the whole TU parsed.
    Matcher.matchAST(Context);
  }

 private:
  OutputStreams &OS;
  classMatchHandler HandlerForClassMatcher;

  MatchFinder Matcher;
};

// For each source file provided to the tool, a new FrontendAction is created.
class MyFrontendAction : public ASTFrontendAction {
 public:
  MyFrontendAction() {
    OS.HeaderOS << "#ifndef UTHREADS_CWRAPPER_H\n"
                   "#define UTHREADS_CWRAPPER_H_\n"
                   "#include <pthread.h>\n"
                   "#include <sys/types.h>\n"
                   "#include <sys/socket.h>\n"
                   "#include <inttypes.h>\n\n"

                   "#ifdef __cplusplus\n"
                   "extern \"C\"{\n"
                   "#endif\n"
                   "#include <stdbool.h>\n";
    OS.BodyOS << "#include \"generic/basics.h\"\n"
                 "#include \"cwrapper.h\"\n"
                 "#include \"runtime/uThread.h\"\n"
                 "#include \"runtime/uThreadPool.h\"\n"
                 "#include \"runtime/kThread.h\"\n"
                 "#include \"io/Network.h\"\n"
                 "#ifdef __cplusplus\n"
                 "extern \"C\"{\n"
                 "#endif\n";
  }

  void EndSourceFileAction() override {
    StringRef headerFile("cwrapper.h");
    StringRef bodyFile("cwrapper.cpp");

    // Open the output file
    std::error_code EC;
    llvm::raw_fd_ostream HOS(headerFile, EC, llvm::sys::fs::OF_None);
    if (EC) {
      llvm::errs() << "while opening '" << headerFile << "': " << EC.message()
                   << '\n';
      exit(1);
    }
    llvm::raw_fd_ostream BOS(bodyFile, EC, llvm::sys::fs::OF_None);
    if (EC) {
      llvm::errs() << "while opening '" << bodyFile << "': " << EC.message()
                   << '\n';
      exit(1);
    }

    OS.HeaderOS << "#ifdef __cplusplus\n"
                   "}\n"
                   "#endif\n"
                   "#endif /* UTHREADS_CWRAPPER_H_ */\n";

    OS.BodyOS << "#ifdef __cplusplus\n"
                 "}\n"
                 "#endif\n";

    OS.HeaderOS.flush();
    OS.BodyOS.flush();
    HOS << OS.headerString << "\n";
    BOS << OS.bodyString << "\n";
  }

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef file) override {
    return std::make_unique<MyASTConsumer>(OS);
  }

 private:
  OutputStreams OS;
};

int main(int argc, const char **argv) {
  // parse the command-line args passed to your code
  auto op = CommonOptionsParser::create(argc, argv, CPP2CCategory);
  assert(op);
  // create a new Clang Tool instance (a LibTooling environment)
  ClangTool Tool(op.get().getCompilations(), op.get().getSourcePathList());

  llvm::SplitString(ClassesToGenrate, ClassList, " ");
  // run the Clang Tool, creating a new FrontendAction
  return Tool.run(newFrontendActionFactory<MyFrontendAction>().get());
}
