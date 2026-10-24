#pragma once
#include "render/drawing_painter.h"

#include <QQuickPaintedItem>
#include <QTransform>
#include <qqmlintegration.h>

// FR-19 sheet preview: the drawing painter on a paper-white page fitted into the item. Click selects rig objects (also
// in the viewport) and drags move them; dimensions and notes are picked, dragged and deleted here. Every edit is one
// undo step. Shift-drag adds a dimension, the note tool places a note, the wheel zooms and middle drag pans.
class DrawingItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString kind MEMBER kind_ NOTIFY kindChanged)                               // plan, front, side
    Q_PROPERTY(QString tool MEMBER tool_ NOTIFY toolChanged)                               // select or note
    Q_PROPERTY(int selectedDimension READ selected_dimension NOTIFY sheetSelectionChanged) // -1 for none
    Q_PROPERTY(int selectedNote READ selected_note NOTIFY sheetSelectionChanged)           // -1 for none
    Q_PROPERTY(double zoom READ zoom NOTIFY viewChanged)                                   // 1 fits the sheet

  public:
    explicit DrawingItem(QQuickItem *parent = nullptr);
    void paint(QPainter *p) override;
    int selected_dimension() const { return dimension_; }
    int selected_note() const { return note_; }
    double zoom() const { return zoom_; }
    Q_INVOKABLE void fit();
    Q_INVOKABLE void deleteSelected(); // the selected dimension or note, else the selected objects

  Q_SIGNALS:
    void kindChanged();
    void toolChanged();
    void sheetSelectionChanged();
    void viewChanged();
    // Asks QML for a note's text: index -1 is a new note (`note` holds at/to), item pixels for the editor.
    void editNote(int index, const QVariantMap &note, QPointF itemPos);

  protected:
    bool event(QEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

  private:
    enum class Drag { none, pan, new_dimension, dimension_end, dimension_offset, note, objects, new_note };
    void select_sheet(int dimension, int note);
    QPointF to_drawing(QPointF item) const; // item pixels -> drawing metres
    std::string hit(QPointF item) const;    // smallest hit rect under the point, "" for none
    QVector3D world_delta(QPointF item, bool free) const; // false snaps to 5 cm
    void write(const char *key, const sv::json &list);

    QString kind_ = QStringLiteral("plan"), tool_ = QStringLiteral("select");
    int dimension_ = -1, note_ = -1;
    double zoom_ = 1;
    QPointF pan_;                              // item pixels, applied after the fit
    QTransform to_item_, page_;                // drawing metres / page mm -> item pixels, from the last paint
    std::vector<sv::render::DrawingHit> hits_; // item pixels
    Drag drag_ = Drag::none;
    QPointF press_, cur_;
    int end_ = 0;         // dimension end being dragged: 0 = a, 1 = b
    std::string grabbed_; // object uuid under the press
    sv::json preview_;    // dimensions or notes while a drag edits them
};
