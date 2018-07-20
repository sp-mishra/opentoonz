

#include "toonzqt/schematicviewer.h"

// TnzQt includes
#include "toonzqt/schematicnode.h"
#include "toonzqt/fxschematicnode.h"
#include "toonzqt/schematicgroupeditor.h"
#include "toonzqt/stageschematicscene.h"
#include "toonzqt/fxschematicscene.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/tselectionhandle.h"
#include "toonzqt/gutil.h"
#include "toonzqt/imageutils.h"
#include "toonzqt/dvscrollwidget.h"

// TnzLib includes
#include "toonz/txsheethandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tobjecthandle.h"
#include "toonz/tfxhandle.h"
#include "toonz/txsheet.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/tcolumnfx.h"
#include "toonz/txshzeraryfxcolumn.h"
#include "toonz/preferences.h"
#include "toonz/fxdag.h"
#include "toonz/tapplication.h"
#include "toonz/tscenehandle.h"

#include "../toonz/menubarcommandids.h"

// Qt includes
#include <QGraphicsSceneMouseEvent>
#include <QMouseEvent>
#include <QGraphicsItem>
#include <QToolBar>
#include <QToolButton>
#include <QMenu>
#include <QIcon>
#include <QAction>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QGestureEvent>

// STD includes
#include "assert.h"
#include "math.h"

namespace {

class SchematicZoomer final : public ImageUtils::ShortcutZoomer {
public:
  SchematicZoomer(QWidget *parent) : ShortcutZoomer(parent) {}

  bool zoom(bool zoomin, bool resetZoom) override {
    static_cast<SchematicSceneViewer *>(getWidget())->zoomQt(zoomin, resetZoom);
    return true;
  }
};

}  // namespace

//==================================================================
//
// SchematicScene
//
//==================================================================

SchematicScene::SchematicScene(QWidget *parent) : QGraphicsScene(parent) {
  setSceneRect(0, 0, 50000, 50000);
  setItemIndexMethod(NoIndex);
}

//------------------------------------------------------------------

SchematicScene::~SchematicScene() { clearAllItems(); }

//------------------------------------------------------------------

void SchematicScene::showEvent(QShowEvent *se) {
  TSelectionHandle *selHandle = TSelectionHandle::getCurrent();
  connect(selHandle, SIGNAL(selectionSwitched(TSelection *, TSelection *)),
          this, SLOT(onSelectionSwitched(TSelection *, TSelection *)));
  clearSelection();
}

//------------------------------------------------------------------

void SchematicScene::hideEvent(QHideEvent *se) {
  TSelectionHandle *selHandle = TSelectionHandle::getCurrent();
  disconnect(selHandle, SIGNAL(selectionSwitched(TSelection *, TSelection *)),
             this, SLOT(onSelectionSwitched(TSelection *, TSelection *)));
}

//------------------------------------------------------------------

/*! Removes and then deletes all item in the scene.
*/

void SchematicScene::clearAllItems() {
  clearSelection();
  m_highlightedLinks.clear();
  QList<SchematicWindowEditor *> editors;
  QList<SchematicNode *> nodes;
  QList<SchematicLink *> links;
  int i;
  QList<QGraphicsItem *> sceneItems = items();
  int size                          = sceneItems.size();
  // create nodes and links list
  for (i = 0; i < size; i++) {
    QGraphicsItem *item           = sceneItems.at(i);
    SchematicWindowEditor *editor = dynamic_cast<SchematicWindowEditor *>(item);
    SchematicNode *node           = dynamic_cast<SchematicNode *>(item);
    SchematicLink *link           = dynamic_cast<SchematicLink *>(item);
    if (editor) editors.append(editor);
    if (node) nodes.append(node);
    if (link) links.append(link);
  }
  while (links.size() > 0) {
    SchematicLink *link = links.back();
    removeItem(link);
    links.removeLast();
    SchematicPort *startPort = link->getStartPort();
    SchematicPort *endPort   = link->getEndPort();
    if (startPort) startPort->removeLink(link);
    if (endPort) endPort->removeLink(link);
    delete link;
  }
  while (editors.size() > 0) {
    SchematicWindowEditor *editor = editors.back();
    removeItem(editor);
    editors.removeLast();
    delete editor;
  }
  while (nodes.size() > 0) {
    SchematicNode *node = nodes.back();
    removeItem(node);
    nodes.removeLast();
    delete node;
  }
  assert(items().size() == 0);
}

//------------------------------------------------------------------
/*! check if any item exists in the rect
*/
bool SchematicScene::isAnEmptyZone(const QRectF &rect) {
  QList<QGraphicsItem *> allItems = items();
  for (auto const level : allItems) {
    SchematicNode *node = dynamic_cast<SchematicNode *>(level);
    if (!node) continue;
    FxSchematicNode *fxNode = dynamic_cast<FxSchematicNode *>(node);
    if (fxNode && fxNode->isA(eXSheetFx)) continue;
    if (node->boundingRect().translated(node->scenePos()).intersects(rect))
      return false;
  }
  return true;
}

//------------------------------------------------------------------

QVector<SchematicNode *> SchematicScene::getPlacedNode(SchematicNode *node) {
  QRectF rect = node->boundingRect().translated(node->scenePos());
  QList<QGraphicsItem *> allItems = items();
  QVector<SchematicNode *> nodes;
  for (auto const item : allItems) {
    SchematicNode *placedNode = dynamic_cast<SchematicNode *>(item);
    if (!placedNode || placedNode == node) continue;
    QRectF nodeRect =
        placedNode->boundingRect().translated(placedNode->scenePos());
    QRectF enlargedRect = rect.adjusted(-10, -10, 10, 10);
    if (enlargedRect.contains(nodeRect)) nodes.push_back(placedNode);
  }
  return nodes;
}

//==================================================================
//
// SchematicSceneViewer
//
//==================================================================

SchematicSceneViewer::SchematicSceneViewer(QWidget *parent)
    : QGraphicsView(parent)
    , m_buttonState(Qt::NoButton)
    , m_oldWinPos()
    , m_oldScenePos()
    , m_firstShowing(true) {
  setObjectName("SchematicSceneViewer");

  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setDragMode(QGraphicsView::NoDrag);
  setTransformationAnchor(QGraphicsView::NoAnchor);
  setRenderHint(QPainter::SmoothPixmapTransform);
  setRenderHint(QPainter::TextAntialiasing);
  setRenderHint(QPainter::Antialiasing);
  setInteractive(true);
  setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
  show();

  setAttribute(Qt::WA_AcceptTouchEvents);
  grabGesture(Qt::SwipeGesture);
  grabGesture(Qt::PanGesture);
  grabGesture(Qt::PinchGesture);
}

//------------------------------------------------------------------

SchematicSceneViewer::~SchematicSceneViewer() {}

//------------------------------------------------------------------

/*! Reimplemets the QGraphicsView::mousePressEvent()
*/
void SchematicSceneViewer::mousePressEvent(QMouseEvent *me) {
  if (m_gestureActive && m_touchDevice == QTouchDevice::TouchScreen) {
    return;
  }

  m_buttonState = me->button();
  m_oldWinPos = m_prevWinPos = me->pos();
  m_oldScenePos              = mapToScene(m_oldWinPos);

  if (m_buttonState == Qt::LeftButton) {
    if (m_cursorMode == CursorMode::Zoom) {
      m_zooming = true;
      return;
    } else if (m_cursorMode == CursorMode::Hand) {
      m_panning = true;
      return;
    }
  }

  bool drawRect                       = true;
  QList<QGraphicsItem *> pointedItems = items(me->pos());
  int i;
  for (i = 0; i < pointedItems.size(); i++) {
    SchematicWindowEditor *editor =
        dynamic_cast<SchematicWindowEditor *>(pointedItems[i]);
    if (!editor) {
      drawRect = false;
      break;
    }
  }

  if (m_buttonState == Qt::LeftButton && drawRect)
    setDragMode(QGraphicsView::RubberBandDrag);
  QGraphicsView::mousePressEvent(me);
}

//------------------------------------------------------------------

/*! Reimplemets the QGraphicsView::mouseMoveEvent()
*/
void SchematicSceneViewer::mouseMoveEvent(QMouseEvent *me) {
  if (m_gestureActive && m_touchDevice == QTouchDevice::TouchScreen) {
    return;
  }

  QPoint currWinPos    = me->pos();
  QPointF currScenePos = mapToScene(currWinPos);
  if ((m_cursorMode == CursorMode::Hand && m_panning) ||
      m_buttonState == Qt::MidButton) {
    panQt(currWinPos);
  } else if (m_cursorMode == CursorMode::Zoom && m_zooming) {
    int deltaY     = (m_prevWinPos.y() - me->pos().y()) * 10;
    double factorY = exp(deltaY * 0.001);
    changeScale(m_oldWinPos, factorY);
    m_prevWinPos = me->pos();
  } else {
    m_oldWinPos   = currWinPos;
    m_oldScenePos = currScenePos;
  }
  QGraphicsView::mouseMoveEvent(me);
}

//------------------------------------------------------------------

/*! Reimplemets the QGraphicsView::mouseReleaseEvent()
*/
void SchematicSceneViewer::mouseReleaseEvent(QMouseEvent *me) {
  // for touchscreens but not touchpads...
  if (m_gestureActive && m_touchDevice == QTouchDevice::TouchScreen) {
    m_gestureActive = false;
    m_zooming       = false;
    m_panning       = false;
    m_scaleFactor   = 0.0;
    return;
  }

  m_zooming = false;
  m_panning = false;

  m_buttonState = Qt::NoButton;
  QGraphicsView::mouseReleaseEvent(me);
  setDragMode(QGraphicsView::NoDrag);
  // update();
}

//------------------------------------------------------------------

void SchematicSceneViewer::mouseDoubleClickEvent(QMouseEvent *event) {
  if (m_gestureActive && m_touchDevice == QTouchDevice::TouchScreen) {
    fitScene();
    m_gestureActive = false;
    return;
  }
  QGraphicsView::mouseDoubleClickEvent(event);
}
//------------------------------------------------------------------

void SchematicSceneViewer::keyPressEvent(QKeyEvent *ke) {
  ke->ignore();
  QGraphicsView::keyPressEvent(ke);
  if (!ke->isAccepted()) SchematicZoomer(this).exec(ke);
}

//------------------------------------------------------------------

/*! Reimplemets the QGraphicsView::wheelEvent()
*/
void SchematicSceneViewer::wheelEvent(QWheelEvent *me) {
  me->accept();
  double factor = exp(me->delta() * 0.001);
  changeScale(me->pos(), factor);
}

//------------------------------------------------------------------

void SchematicSceneViewer::zoomQt(bool zoomin, bool resetZoom) {
#if QT_VERSION >= 0x050000
  double scale2 = matrix().determinant();
#else
  double scale2 = matrix().det();
#endif

  // if the zoom scale is changed over 100% in FxSchematic, call updateScene
  bool beforeIsLarge = (scale2 >= 1.0);

  if (resetZoom ||
      ((scale2 < 100000 || !zoomin) && (scale2 > 0.001 * 0.05 || zoomin))) {
    double oldZoomScale = sqrt(scale2);
    double zoomScale    = resetZoom ? 1 : ImageUtils::getQuantizedZoomFactor(
                                           oldZoomScale, zoomin);
    QMatrix scale =
        QMatrix().scale(zoomScale / oldZoomScale, zoomScale / oldZoomScale);

    // See QGraphicsView::mapToScene()'s doc for details
    QRect rect(0, 0, width(), height());
    QRectF sceneCenterRect(
        mapToScene(QRect(rect.center(), QSize(2, 2))).boundingRect());
    setMatrix(scale, true);
    centerOn(sceneCenterRect.center());

#if QT_VERSION >= 0x050000
    bool afterIsLarge = (matrix().determinant() >= 1.0);
#else
    bool afterIsLarge  = (matrix().det() >= 1.0);
#endif
    if (beforeIsLarge != afterIsLarge) {
      FxSchematicScene *fxScene = qobject_cast<FxSchematicScene *>(scene());
      if (fxScene) fxScene->updateScene();
    }
  }
}

//------------------------------------------------------------------

/*! The view is scaled around the point \b winPos by \b scaleFactor;
*/
void SchematicSceneViewer::changeScale(const QPoint &winPos,
                                       qreal scaleFactor) {
#if QT_VERSION >= 0x050000
  bool beforeIsLarge = (matrix().determinant() >= 1.0);
#else
  bool beforeIsLarge   = (matrix().det() >= 1.0);
#endif
  QPointF startScenePos = mapToScene(winPos);
  QMatrix scale         = QMatrix().scale(scaleFactor, scaleFactor);
  setMatrix(scale, true);
  QPointF endScenePos = mapToScene(winPos);
  QPointF delta       = endScenePos - startScenePos;
  translate(delta.x(), delta.y());

#if QT_VERSION >= 0x050000
  bool afterIsLarge = (matrix().determinant() >= 1.0);
#else
  bool afterIsLarge    = (matrix().det() >= 1.0);
#endif
  if (beforeIsLarge != afterIsLarge) {
    FxSchematicScene *fxScene = qobject_cast<FxSchematicScene *>(scene());
    if (fxScene) fxScene->updateScene();
  }
}

//------------------------------------------------------------------

void SchematicSceneViewer::fitScene() {
  if (scene()) {
#if QT_VERSION >= 0x050000
    bool beforeIsLarge = (matrix().determinant() >= 1.0);
#else
    bool beforeIsLarge = (matrix().det() >= 1.0);
#endif

    QRectF rect = scene()->itemsBoundingRect();
    fitInView(rect, Qt::KeepAspectRatio);

#if QT_VERSION >= 0x050000
    bool afterIsLarge = (matrix().determinant() >= 1.0);
#else
    bool afterIsLarge  = (matrix().det() >= 1.0);
#endif
    if (beforeIsLarge != afterIsLarge) {
      FxSchematicScene *fxScene = qobject_cast<FxSchematicScene *>(scene());
      if (fxScene) fxScene->updateScene();
    }
  }
}

//------------------------------------------------------------------

void SchematicSceneViewer::centerOnCurrent() {
  SchematicScene *schematicScene = dynamic_cast<SchematicScene *>(scene());
  QGraphicsItem *node            = schematicScene->getCurrentNode();
  if (node) centerOn(node);
}

//------------------------------------------------------------------

void SchematicSceneViewer::reorderScene() {
  SchematicScene *schematicScene = dynamic_cast<SchematicScene *>(scene());
  schematicScene->reorderScene();
}

//------------------------------------------------------------------

void SchematicSceneViewer::normalizeScene() {
#if QT_VERSION >= 0x050000
  bool beforeIsLarge = (matrix().determinant() >= 1.0);
#else
  bool beforeIsLarge   = (matrix().det() >= 1.0);
#endif
  // See QGraphicsView::mapToScene()'s doc for details
  QRect rect(0, 0, width(), height());
  QRectF sceneCenterRect(
      mapToScene(QRect(rect.center(), QSize(2, 2))).boundingRect());
  resetMatrix();
#if defined(MACOSX)
  scale(1.32, 1.32);
#endif
  centerOn(sceneCenterRect.center());

#if QT_VERSION >= 0x050000
  bool afterIsLarge = (matrix().determinant() >= 1.0);
#else
  bool afterIsLarge    = (matrix().det() >= 1.0);
#endif
  if (beforeIsLarge != afterIsLarge) {
    FxSchematicScene *fxScene = qobject_cast<FxSchematicScene *>(scene());
    if (fxScene) fxScene->updateScene();
  }
}

//------------------------------------------------------------------
void SchematicSceneViewer::panQt(QPoint currWinPos) {
  QPointF currScenePos = mapToScene(currWinPos);

  setInteractive(false);
  // I need to disable QGraphicsView event handling to avoid the generation of
  // 'virtual' mouseMoveEvent
  QPointF deltaPoint = currScenePos - m_oldScenePos;
  translate(deltaPoint.x(), deltaPoint.y());
  currScenePos = mapToScene(currWinPos);
  // translate has changed the matrix affecting the mapToScene() method. I
  // have to recompute currScenePos
  setInteractive(true);

  m_oldWinPos   = currWinPos;
  m_oldScenePos = currScenePos;
}

void SchematicSceneViewer::showEvent(QShowEvent *se) {
  QGraphicsView::showEvent(se);
  if (m_firstShowing) {
    m_firstShowing = false;
    QRectF rect    = scene()->itemsBoundingRect();
    resetMatrix();
    centerOn(rect.center());
  }
}

//------------------------------------------------------------------
void SchematicSceneViewer::touchEvent(QTouchEvent *e, int type) {
  if (type == QEvent::TouchBegin) {
    m_touchActive = true;
    m_oldWinPos   = e->touchPoints().at(0).pos().toPoint();
    m_oldScenePos = mapToScene(m_oldWinPos);
    // obtain device type
    m_touchDevice = e->device()->type();
  } else if (m_touchActive) {
    // touchpads must have 2 finger panning for tools and navigation to be
    // functional
    // on other devices, 1 finger panning is preferred
    if ((e->touchPoints().count() == 2 &&
         m_touchDevice == QTouchDevice::TouchPad) ||
        (e->touchPoints().count() == 1 &&
         m_touchDevice == QTouchDevice::TouchScreen)) {
      QTouchEvent::TouchPoint currPos = e->touchPoints().at(0);
      QPoint currWinPos               = currPos.pos().toPoint();
      if (!m_panning) {
        QPointF deltaPoint = currWinPos - m_oldWinPos;
        // minimize accidental and jerky zooming/rotating during 2 finger
        // panning
        if ((deltaPoint.manhattanLength() > 100) && !m_zooming) {
          m_panning = true;
        }
      }
      if (m_panning) {
        m_oldWinPos   = currPos.lastPos().toPoint();
        m_oldScenePos = mapToScene(m_oldWinPos);
        panQt(currWinPos);
      }
    }
  }
  if (type == QEvent::TouchEnd || type == QEvent::TouchCancel) {
    m_touchActive = false;
    m_panning     = false;
  }
  e->accept();
}

//------------------------------------------------------------------

void SchematicSceneViewer::gestureEvent(QGestureEvent *e) {
  m_gestureActive = false;
  if (QGesture *swipe = e->gesture(Qt::SwipeGesture)) {
    m_gestureActive = true;
  } else if (QGesture *pan = e->gesture(Qt::PanGesture)) {
    m_gestureActive = true;
  }
  if (QGesture *pinch = e->gesture(Qt::PinchGesture)) {
    QPinchGesture *gesture = static_cast<QPinchGesture *>(pinch);
    QPinchGesture::ChangeFlags changeFlags = gesture->changeFlags();
    QPoint currCenter                      = gesture->centerPoint().toPoint();
    QPoint currWinCenter                   = mapFromGlobal(currCenter);

    if (gesture->state() == Qt::GestureStarted) {
      m_oldWinPos = m_prevWinPos = currWinCenter;
      m_oldScenePos              = mapToScene(m_oldWinPos);
      m_gestureActive            = true;
    } else if (gesture->state() == Qt::GestureFinished) {
      m_oldWinPos = m_prevWinPos = currWinCenter;
      m_oldScenePos              = mapToScene(m_oldWinPos);
      m_gestureActive            = false;
      m_zooming                  = false;
      m_scaleFactor              = 0.0;
    } else {
      if (changeFlags & QPinchGesture::ScaleFactorChanged) {
        double scaleFactor = gesture->scaleFactor();
        // the scale factor makes for too sensitive scaling
        // divide the change in half
        if (scaleFactor > 1) {
          double decimalValue = scaleFactor - 1;
          decimalValue /= 1.5;
          scaleFactor = 1 + decimalValue;
        } else if (scaleFactor < 1) {
          double decimalValue = 1 - scaleFactor;
          decimalValue /= 1.5;
          scaleFactor = 1 - decimalValue;
        }

        if (!m_zooming) {
          double delta = scaleFactor - 1;
          m_scaleFactor += delta;
          if (m_scaleFactor > .2 || m_scaleFactor < -.2) {
            m_zooming = true;
          }
        }
        if (m_zooming) {
          changeScale(m_oldWinPos, scaleFactor);
          m_prevWinPos = currWinCenter;
        }
        m_gestureActive = true;
      }

      if (changeFlags & QPinchGesture::CenterPointChanged) {
        QPointF deltaPoint =
            gesture->centerPoint() - gesture->lastCenterPoint();
        if (deltaPoint.manhattanLength() > 1) {
          // panQt(currWinCenter);
        }
        m_gestureActive = true;
      }
    }
  }
  e->accept();
}

//------------------------------------------------------------------

bool SchematicSceneViewer::event(QEvent *e) {
  if (CommandManager::instance()
          ->getAction(MI_TouchGestureControl)
          ->isChecked()) {
    if (e->type() == QEvent::Gesture) {
      gestureEvent(static_cast<QGestureEvent *>(e));
      return true;
    }
    if (e->type() == QEvent::TouchBegin || e->type() == QEvent::TouchEnd ||
        e->type() == QEvent::TouchCancel || e->type() == QEvent::TouchUpdate) {
      touchEvent(static_cast<QTouchEvent *>(e), e->type());
      m_gestureActive = true;
      return true;
    }
  }
  return QGraphicsView::event(e);
}

//------------------------------------------------------------------

void SchematicSceneViewer::setCursorMode(CursorMode cursorMode) {
  m_cursorMode = cursorMode;
}

//==================================================================
//
// SchematicViewer
//
//==================================================================

SchematicViewer::SchematicViewer(QWidget *parent)
    : QWidget(parent)
    , m_fullSchematic(true)
    , m_maximizedNode(false)
    , m_sceneHandle(0)
    , m_cursorMode(CursorMode::Select) {
  m_viewer     = new SchematicSceneViewer(this);
  m_stageScene = new StageSchematicScene(this);
  m_fxScene    = new FxSchematicScene(this);

  m_commonToolbar = new QToolBar(m_viewer);
  m_stageToolbar  = new QToolBar(m_viewer);
  m_fxToolbar     = new QToolBar(m_viewer);
  m_swapToolbar   = new QToolBar(m_viewer);

  m_commonToolbar->setObjectName("MediumPaddingToolBar");
  m_stageToolbar->setObjectName("MediumPaddingToolBar");
  m_fxToolbar->setObjectName("MediumPaddingToolBar");
  m_swapToolbar->setObjectName("MediumPaddingToolBar");

  createToolbars();
  createActions();

  // layout
  QVBoxLayout *mainLayout = new QVBoxLayout();
  mainLayout->setMargin(0);
  mainLayout->setSpacing(0);
  {
    mainLayout->addWidget(m_viewer, 1);

    QFrame *bottomFrame = new QFrame(this);
    bottomFrame->setObjectName("SchematicBottomFrame");
    QHBoxLayout *horizontalLayout = new QHBoxLayout();
    horizontalLayout->setMargin(0);
    horizontalLayout->setSpacing(0);
    {
      horizontalLayout->addWidget(m_commonToolbar);
      horizontalLayout->addStretch();
      horizontalLayout->addWidget(m_fxToolbar);
      horizontalLayout->addWidget(m_stageToolbar);
      horizontalLayout->addWidget(m_swapToolbar);
    }
    bottomFrame->setLayout(horizontalLayout);

    mainLayout->addWidget(bottomFrame, 0);
  }
  setLayout(mainLayout);

  connect(m_fxScene, SIGNAL(showPreview(TFxP)), this,
          SIGNAL(showPreview(TFxP)));
  connect(m_fxScene, SIGNAL(doCollapse(const QList<TFxP> &)), this,
          SIGNAL(doCollapse(const QList<TFxP> &)));
  connect(m_stageScene, SIGNAL(doCollapse(QList<TStageObjectId>)), this,
          SIGNAL(doCollapse(QList<TStageObjectId>)));
  connect(m_fxScene, SIGNAL(doExplodeChild(const QList<TFxP> &)), this,
          SIGNAL(doExplodeChild(const QList<TFxP> &)));
  connect(m_stageScene, SIGNAL(doExplodeChild(QList<TStageObjectId>)), this,
          SIGNAL(doExplodeChild(QList<TStageObjectId>)));
  connect(m_stageScene, SIGNAL(editObject()), this, SIGNAL(editObject()));
  connect(m_fxScene, SIGNAL(editObject()), this, SIGNAL(editObject()));

  m_viewer->setScene(m_stageScene);
  m_fxToolbar->hide();

  setFocusProxy(m_viewer);
}

//------------------------------------------------------------------

SchematicViewer::~SchematicViewer() {}

//------------------------------------------------------------------

void SchematicViewer::setApplication(TApplication *app) {
  m_stageScene->setXsheetHandle(app->getCurrentXsheet());
  m_stageScene->setObjectHandle(app->getCurrentObject());
  m_stageScene->setFxHandle(app->getCurrentFx());
  m_stageScene->setColumnHandle(app->getCurrentColumn());
  m_stageScene->setSceneHandle(app->getCurrentScene());
  m_stageScene->setFrameHandle(app->getCurrentFrame());

  m_fxScene->setApplication(app);
}

//------------------------------------------------------------------

void SchematicViewer::updateSchematic() {
  m_stageScene->updateScene();
  m_fxScene->updateScene();
}

//------------------------------------------------------------------

void SchematicViewer::setSchematicScene(SchematicScene *scene) {
  if (scene) {
    m_viewer->setScene(scene);
    m_viewer->centerOn(scene->sceneRect().center());
  }
}

//------------------------------------------------------------------

void SchematicViewer::createToolbars() {
  // Initialize them
  m_stageToolbar->setMovable(false);
  m_stageToolbar->setIconSize(QSize(17, 17));
  m_stageToolbar->setLayoutDirection(Qt::RightToLeft);
  m_stageToolbar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

  m_commonToolbar->setMovable(false);
  m_commonToolbar->setIconSize(QSize(17, 17));
  m_commonToolbar->setLayoutDirection(Qt::RightToLeft);
  m_commonToolbar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

  m_fxToolbar->setMovable(false);
  m_fxToolbar->setIconSize(QSize(17, 17));
  m_fxToolbar->setLayoutDirection(Qt::RightToLeft);
  m_fxToolbar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

  m_swapToolbar->setMovable(false);
  m_swapToolbar->setIconSize(QSize(17, 17));
  m_swapToolbar->setLayoutDirection(Qt::RightToLeft);
  m_swapToolbar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

//------------------------------------------------------------------

void SchematicViewer::createActions() {
  // Create all actions
  QAction *addPegbar = 0, *addSpline = 0, *addCamera = 0, *insertFx = 0,
          *addOutputFx = 0, *switchPort = 0;
  {
    // Fit schematic
    QIcon fitSchematicIcon = createQIconOnOff("fit", false);
    m_fitSchematic =
        new QAction(fitSchematicIcon, tr("&Fit to Window"), m_commonToolbar);
    connect(m_fitSchematic, SIGNAL(triggered()), m_viewer, SLOT(fitScene()));

    // Center On
    QIcon centerOnIcon = createQIconOnOff("centerselection", false);
    m_centerOn =
        new QAction(centerOnIcon, tr("&Focus on Current"), m_commonToolbar);
    connect(m_centerOn, SIGNAL(triggered()), m_viewer, SLOT(centerOnCurrent()));

    // Reorder schematic
    QIcon reorderIcon = createQIconOnOff("reorder", false);
    m_reorder = new QAction(reorderIcon, tr("&Reorder Nodes"), m_commonToolbar);
    connect(m_reorder, SIGNAL(triggered()), m_viewer, SLOT(reorderScene()));

    // Normalize schematic schematic
    QIcon normalizeIcon = createQIconOnOff("resetsize", false);
    m_normalize =
        new QAction(normalizeIcon, tr("&Reset Size"), m_commonToolbar);
    connect(m_normalize, SIGNAL(triggered()), m_viewer, SLOT(normalizeScene()));

    QIcon nodeSizeIcon = createQIconOnOff(
        m_maximizedNode ? "minimizenodes" : "maximizenodes", false);
    m_nodeSize =
        new QAction(nodeSizeIcon, m_maximizedNode ? tr("&Minimize Nodes")
                                                  : tr("&Maximize Nodes"),
                    m_commonToolbar);
    connect(m_nodeSize, SIGNAL(triggered()), this, SLOT(changeNodeSize()));

    QIcon selectModeIcon = createQIconOnOff("schematic_selection_mode", false);
    m_selectMode =
        new QAction(selectModeIcon, tr("&Selection Mode"), m_commonToolbar);
    m_selectMode->setCheckable(true);
    connect(m_selectMode, SIGNAL(triggered()), this, SLOT(selectModeEnabled()));

    QIcon zoomModeIcon = createQIconOnOff("schematic_zoom_mode", false);
    m_zoomMode = new QAction(zoomModeIcon, tr("&Zoom Mode"), m_commonToolbar);
    m_zoomMode->setCheckable(true);
    connect(m_zoomMode, SIGNAL(triggered()), this, SLOT(zoomModeEnabled()));

    QIcon handModeIcon = createQIconOnOff("schematic_hand_mode", false);
    m_handMode = new QAction(handModeIcon, tr("&Hand Mode"), m_commonToolbar);
    m_handMode->setCheckable(true);
    connect(m_handMode, SIGNAL(triggered()), this, SLOT(handModeEnabled()));

    setCursorMode(m_cursorMode);

    if (m_fullSchematic) {
      // AddPegbar
      addPegbar           = new QAction(tr("&New Pegbar"), m_stageToolbar);
      QIcon addPegbarIcon = createQIconOnOff("pegbar", false);
      addPegbar->setIcon(addPegbarIcon);
      connect(addPegbar, SIGNAL(triggered()), m_stageScene,
              SLOT(onPegbarAdded()));

      // AddCamera
      addCamera           = new QAction(tr("&New Camera"), m_stageToolbar);
      QIcon addCameraIcon = createQIconOnOff("camera", false);
      addCamera->setIcon(addCameraIcon);
      connect(addCamera, SIGNAL(triggered()), m_stageScene,
              SLOT(onCameraAdded()));

      // AddSpline
      addSpline           = new QAction(tr("&New Motion Path"), m_stageToolbar);
      QIcon addSplineIcon = createQIconOnOff("motionpath", false);
      addSpline->setIcon(addSplineIcon);
      connect(addSpline, SIGNAL(triggered()), m_stageScene,
              SLOT(onSplineAdded()));

      // Switch display of stage schematic's output port
      switchPort =
          new QAction(tr("&Swtich output port display mode"), m_stageToolbar);
      switchPort->setCheckable(true);
      switchPort->setChecked(m_stageScene->isShowLetterOnPortFlagEnabled());
      QIcon switchPortIcon = createQIconOnOff("switchport");
      switchPort->setIcon(switchPortIcon);
      connect(switchPort, SIGNAL(toggled(bool)), m_stageScene,
              SLOT(onSwitchPortModeToggled(bool)));

      // InsertFx
      insertFx = CommandManager::instance()->getAction("MI_InsertFx");
      if (insertFx) {
        QIcon insertFxIcon = createQIconOnOff("fx", false);
        insertFx->setIcon(insertFxIcon);
      }

      // AddOutputFx
      addOutputFx = CommandManager::instance()->getAction("MI_NewOutputFx");

      // Swap fx/stage schematic
      QIcon changeSchematicIcon = createQIconOnOff("swap", false);
      m_changeScene =
          CommandManager::instance()->getAction("A_FxSchematicToggle", true);
      if (m_changeScene) {
        m_changeScene->setIcon(changeSchematicIcon);
        connect(m_changeScene, SIGNAL(triggered()), this,
                SLOT(onSceneChanged()));
      } else
        m_changeScene = 0;
    }
  }

  // Add actions to toolbars (in reverse)

  m_commonToolbar->addSeparator();
  m_commonToolbar->addAction(m_nodeSize);
  m_commonToolbar->addAction(m_normalize);
  m_commonToolbar->addAction(m_reorder);
  m_commonToolbar->addAction(m_centerOn);
  m_commonToolbar->addAction(m_fitSchematic);
  m_commonToolbar->addSeparator();
  m_commonToolbar->addAction(m_handMode);
  m_commonToolbar->addAction(m_zoomMode);
  m_commonToolbar->addAction(m_selectMode);

  if (m_fullSchematic) {
    m_stageToolbar->addSeparator();
    m_stageToolbar->addAction(switchPort);
    m_stageToolbar->addSeparator();
    m_stageToolbar->addAction(addSpline);
    m_stageToolbar->addAction(addCamera);
    m_stageToolbar->addAction(addPegbar);

    m_fxToolbar->addSeparator();
    m_fxToolbar->addAction(addOutputFx);
    m_fxToolbar->addAction(insertFx);

    if (m_changeScene) m_swapToolbar->addAction(m_changeScene);
  }
}

//------------------------------------------------------------------

void SchematicViewer::setStageSchematic() {
  if (m_viewer->scene() != m_stageScene) {
    m_viewer->setScene(m_stageScene);
    QRectF rect = m_stageScene->itemsBoundingRect();

    m_viewer->resetMatrix();
    m_viewer->centerOn(rect.center());

    m_fxToolbar->hide();
    m_stageToolbar->show();

    m_viewer->update();
  }

  parentWidget()->setWindowTitle(QObject::tr("Stage Schematic"));
}

//------------------------------------------------------------------

void SchematicViewer::setFxSchematic() {
  if (m_viewer->scene() != m_fxScene) {
    m_viewer->setScene(m_fxScene);
    QRectF rect = m_fxScene->itemsBoundingRect();

    m_viewer->resetMatrix();
    m_viewer->centerOn(rect.center());

    m_stageToolbar->hide();
    m_fxToolbar->show();

    // check if the fx scene was small scaled (icon view mode)
    if (!m_fxScene->isLargeScaled()) m_fxScene->updateScene();

    m_viewer->update();
  }

  parentWidget()->setWindowTitle(QObject::tr("FX Schematic"));
}

//------------------------------------------------------------------

void SchematicViewer::onSceneChanged() {
  if (!hasFocus()) return;

  QGraphicsScene *scene = m_viewer->scene();
  if (scene == m_fxScene)
    setStageSchematic();
  else if (scene == m_stageScene)
    setFxSchematic();
}

//------------------------------------------------------------------

void SchematicViewer::onSceneSwitched() {
  m_maximizedNode = m_fxScene->getXsheetHandle()
                        ->getXsheet()
                        ->getFxDag()
                        ->getDagGridDimension() == 0;
  QIcon nodeSizeIcon = createQIconOnOff(
      m_maximizedNode ? "minimizenodes" : "maximizenodes", false);
  m_nodeSize->setIcon(nodeSizeIcon);
  QString label(m_maximizedNode ? tr("&Minimize Nodes")
                                : tr("&Maximize Nodes"));
  m_nodeSize->setText(label);

  // reset schematic
  m_viewer->resetMatrix();
  m_viewer->centerOn(m_viewer->scene()->itemsBoundingRect().center());
  if (m_viewer->scene() == m_fxScene && !m_fxScene->isLargeScaled())
    m_fxScene->updateScene();
}

//------------------------------------------------------------------

bool SchematicViewer::isStageSchematicViewed() {
  QGraphicsScene *scene = m_viewer->scene();
  return scene == m_stageScene;
}

//------------------------------------------------------------------

void SchematicViewer::setStageSchematicViewed(bool isStageSchematic) {
  if (!m_fullSchematic) isStageSchematic = true;

  if (isStageSchematic == isStageSchematicViewed()) return;
  if (isStageSchematic)
    setStageSchematic();
  else
    setFxSchematic();
}

//------------------------------------------------------------------

void SchematicViewer::updateScenes() {
  TStageObjectId id = m_stageScene->getCurrentObject();
  if (id.isColumn()) {
    m_stageScene->update();
    TXsheet *xsh = m_stageScene->getXsheetHandle()->getXsheet();
    if (!xsh) return;
    TXshColumn *column = xsh->getColumn(id.getIndex());
    if (!column) {
      m_fxScene->getFxHandle()->setFx(0, false);
      return;
    }
    TFx *fx = column->getFx();
    m_fxScene->getFxHandle()->setFx(fx, false);
    m_fxScene->update();
  }
}

//------------------------------------------------------------------

void SchematicViewer::changeNodeSize() {
  m_maximizedNode = !m_maximizedNode;
  // aggiono l'icona del pulsante;
  m_fxScene->resizeNodes(m_maximizedNode);
  m_stageScene->resizeNodes(m_maximizedNode);
  QIcon nodeSizeIcon = createQIconOnOff(
      m_maximizedNode ? "minimizenodes" : "maximizenodes", false);
  m_nodeSize->setIcon(nodeSizeIcon);
  QString label(m_maximizedNode ? tr("&Minimize Nodes")
                                : tr("&Maximize Nodes"));
  m_nodeSize->setText(label);
}

//------------------------------------------------------------------

void SchematicViewer::setCursorMode(CursorMode cursorMode) {
  m_cursorMode = cursorMode;
  m_viewer->setCursorMode(m_cursorMode);

  m_selectMode->setChecked((m_cursorMode == CursorMode::Select));
  m_zoomMode->setChecked((m_cursorMode == CursorMode::Zoom));
  m_handMode->setChecked((m_cursorMode == CursorMode::Hand));
}

//------------------------------------------------------------------

void SchematicViewer::selectModeEnabled() { setCursorMode(CursorMode::Select); }

//------------------------------------------------------------------

void SchematicViewer::zoomModeEnabled() { setCursorMode(CursorMode::Zoom); }

//------------------------------------------------------------------

void SchematicViewer::handModeEnabled() { setCursorMode(CursorMode::Hand); }
