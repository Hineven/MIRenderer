/*
 * Created: 2026/1/4
 * Author:  GitHub Copilot
 * See LICENSE for licensing.
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "core/util/command_line.h"
#include "infra_impl/infra.h"

using namespace mi;

class CommandLineTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Force a deterministic resource root for tests.
        root_ = std::filesystem::temp_directory_path() / "mi_cmdline_test_root";
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_ / "assets" / "shaders", ec);
        std::ofstream(root_ / "assets" / "a.txt").put('x');

        TransferInfra(std::make_unique<MyInfra>(false, root_.string()));
        GetInfra().Init();
    }

    void TearDown() override {
        GetInfra().Shutdown();
        DestroyInfra();

        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    std::filesystem::path root_;
};

TEST_F(CommandLineTest, Tokenize_BasicAndQuotesAndEscape) {
    auto r1 = CommandLineTokenize("a  b\t c");
    ASSERT_EQ(r1.tokens.size(), 3u);
    EXPECT_EQ(r1.tokens[0].text, "a");
    EXPECT_EQ(r1.tokens[1].text, "b");
    EXPECT_EQ(r1.tokens[2].text, "c");

    auto r2 = CommandLineTokenize("say \"hello world\"");
    ASSERT_EQ(r2.tokens.size(), 2u);
    EXPECT_EQ(r2.tokens[0].text, "say");
    EXPECT_EQ(r2.tokens[1].text, "hello world");

    // Single-quote escapes next character.
    auto r3 = CommandLineTokenize("a ' b");
    ASSERT_EQ(r3.tokens.size(), 2u);
    EXPECT_EQ(r3.tokens[0].text, "a");
    EXPECT_EQ(r3.tokens[1].text, " b");
}

TEST_F(CommandLineTest, CommandMatch_WithDefaults) {
    Command cmd(
        "set_mode",
        {
            CommandTokenSpec::KeywordSet({"set"}),
            CommandTokenSpec::KeywordSet({"fast", "safe"}, "safe"),
        }
    );

    {
        auto m = cmd.Match({"set"});
        EXPECT_EQ(m.kind, CommandMatchKind::kFull);
        ASSERT_EQ(m.args.size(), 2u);
        EXPECT_EQ(m.args[1], "safe");
    }

    {
        auto m = cmd.Match({"set", "fast"});
        EXPECT_EQ(m.kind, CommandMatchKind::kFull);
        ASSERT_EQ(m.args.size(), 2u);
        EXPECT_EQ(m.args[1], "fast");
    }

    {
        auto m = cmd.Match({"set", "unknown"});
        EXPECT_EQ(m.kind, CommandMatchKind::kNoMatch);
    }
}

TEST_F(CommandLineTest, Completion_Path) {
    auto cmd = Command::Make(
        "open",
        {
            CommandTokenSpec::KeywordSet({"open"}),
            CommandTokenSpec::Path(),
        }
    );

    auto & reg = CommandRegistry::Get();
    reg.Register(std::move(cmd));

    CommandLineCompletionRequest req;
    req.line = "open assets/";

    auto r = reg.Complete(req);
    // Expect at least a.txt and shaders/
    bool has_txt = false;
    bool has_dir = false;
    for (const auto &it : r.items) {
        if (it.text == "assets/a.txt") has_txt = true;
        if (it.text == "assets/shaders/") has_dir = true;
    }
    EXPECT_TRUE(has_txt);
    EXPECT_TRUE(has_dir);
}

TEST_F(CommandLineTest, Match_DisambiguateByDeeperKeywordPrefix) {
    // Reset registry for this test (singleton). We can't directly clear it, so we use unique names
    // and only assert that the best match for this input is the one with deeper keyword match.
    auto &reg = CommandRegistry::Get();

    reg.Register(Command::Make(
        "camera_dir_test",
        {
            CommandTokenSpec::KeywordSet({"c"}),
            CommandTokenSpec::KeywordSet({"dir"}),
            CommandTokenSpec::Free({}, "x"),
            CommandTokenSpec::Free({}, "y"),
            CommandTokenSpec::Free({}, "z"),
        }
    ));

    reg.Register(Command::Make(
        "camera_pos_test",
        {
            CommandTokenSpec::KeywordSet({"c"}),
            CommandTokenSpec::KeywordSet({"pos"}),
            CommandTokenSpec::Free({}, "x"),
            CommandTokenSpec::Free({}, "y"),
            CommandTokenSpec::Free({}, "z"),
        }
    ));

    // User typed a valid command prefix `c pos` but not enough args yet.
    auto m = reg.Match("c pos");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, CommandMatchKind::kBadMatch);
    ASSERT_TRUE(m->command != nullptr);
    EXPECT_EQ(m->command->GetName(), "camera_pos_test");
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
