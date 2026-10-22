#include "core/document.h"

#include <catch2/catch_test_macros.hpp>

using sv::json;

static json add_object(const json &o) { return json::array({{{"op", "add"}, {"path", "/objects/-"}, {"value", o}}}); }
static json move_x(int index, double x) {
    return json::array(
        {{{"op", "replace"}, {"path", "/objects/" + std::to_string(index) + "/position/0"}, {"value", x}}});
}

TEST_CASE("FR-26 undo and redo restore exact documents") {
    sv::Document doc;
    const json empty = doc.data();
    doc.apply(add_object(sv::new_truss("Truss A")), "Add truss");
    const json with_truss = doc.data();
    doc.apply(move_x(0, 2.5), "Move truss");
    CHECK(doc.data()["objects"][0]["position"][0] == 2.5);

    doc.undo();
    CHECK(doc.data() == with_truss);
    doc.undo();
    CHECK(doc.data() == empty);
    CHECK_FALSE(doc.can_undo());
    doc.undo(); // no-op at the start
    doc.redo();
    doc.redo();
    CHECK(doc.data()["objects"][0]["position"][0] == 2.5);
    CHECK_FALSE(doc.can_redo());
    REQUIRE(doc.history().size() == 2);
    CHECK(doc.history()[1].label == "Move truss");
}

TEST_CASE("a new edit after undo drops the redo branch") {
    sv::Document doc;
    doc.apply(add_object(sv::new_truss("A")), "Add A");
    doc.apply(move_x(0, 1.0), "Move 1");
    doc.undo();
    doc.apply(move_x(0, 9.0), "Move 9");
    CHECK_FALSE(doc.can_redo());
    CHECK(doc.history().size() == 2);
    CHECK(doc.history()[1].label == "Move 9");
}

TEST_CASE("a patch that does not apply leaves document and history unchanged") {
    sv::Document doc;
    const json before = doc.data();
    CHECK_THROWS(doc.apply(move_x(5, 1.0), "Move missing"));
    CHECK(doc.data() == before);
    CHECK(doc.history().empty());
}

TEST_CASE("dirty tracks the saved point through undo and redo") {
    sv::Document doc;
    CHECK_FALSE(doc.dirty());
    doc.apply(add_object(sv::new_truss("A")), "Add A");
    CHECK(doc.dirty());
    doc.mark_saved();
    CHECK_FALSE(doc.dirty());
    doc.undo();
    CHECK(doc.dirty());
    doc.redo();
    CHECK_FALSE(doc.dirty());
    doc.undo();
    doc.apply(add_object(sv::new_truss("B")), "Add B"); // saved state is gone for good
    doc.undo();
    doc.redo();
    CHECK(doc.dirty());
}
