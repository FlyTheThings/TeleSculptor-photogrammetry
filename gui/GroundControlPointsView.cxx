/*ckwg +29
 * Copyright 2018-2019 by Kitware, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  * Neither the name Kitware, Inc. nor the names of any contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "GroundControlPointsView.h"

#include "ui_GroundControlPointsView.h"
#include "am_GroundControlPointsView.h"

#include "GroundControlPointsHelper.h"
#include "GroundControlPointsModel.h"

#include <vital/types/geodesy.h>

#include <qtScopedValueChange.h>
#include <qtUtil.h>

#include <QClipboard>
#include <QDebug>
#include <QEvent>
#include <QFile>
#include <QMenu>
#include <QPainter>
#include <QScreen>
#include <QSvgRenderer>
#include <QToolButton>
#include <QWindow>

#include <limits>

namespace kv = kwiver::vital;

using id_t = kv::ground_control_point_id_t;

namespace
{

constexpr auto INVALID_POINT = std::numeric_limits<id_t>::max();

//-----------------------------------------------------------------------------
QPixmap colorize(QByteArray svg, int physicalSize, int logicalSize,
                 double devicePixelRatio, QColor color)
{
  svg.replace("#ffffff", color.name().toLatin1());

  QPixmap p{physicalSize, physicalSize};
  p.setDevicePixelRatio(devicePixelRatio);
  p.fill(Qt::transparent);

  QSvgRenderer renderer{svg};
  QPainter painter{&p};
  renderer.render(&painter, QRect{0, 0, logicalSize, logicalSize});

  return p;
}

}

//-----------------------------------------------------------------------------
class GroundControlPointsViewPrivate
{
public:
  void updateRegisteredIcon(QWidget* widget);

  void enableControls(bool state, bool haveLocation = true);

  void showPoint(id_t id);
  void setPointPosition(id_t id);
  id_t selectedPoint() const;

  void copyLocation(bool northingFirst, bool includeElevation);

  Ui::GroundControlPointsView UI;
  Am::GroundControlPointsView AM;

  QMenu* popupMenu;
  QToolButton* copyLocationButton;

  QMetaObject::Connection screenChanged;

  GroundControlPointsModel model;
  GroundControlPointsHelper* helper = nullptr;

  id_t currentPoint = INVALID_POINT;
};

QTE_IMPLEMENT_D_FUNC(GroundControlPointsView)

//-----------------------------------------------------------------------------
void GroundControlPointsViewPrivate::updateRegisteredIcon(QWidget* widget)
{
  QIcon icon;

  auto const& palette = widget->palette();
  auto const normalColor =
    palette.color(QPalette::Active, QPalette::Text);
  auto const selectedColor =
    palette.color(QPalette::Active, QPalette::HighlightedText);
  auto const disabledColor =
    palette.color(QPalette::Disabled, QPalette::Text);

  QFile f{QStringLiteral(":/icons/scalable/registered")};
  f.open(QIODevice::ReadOnly);
  auto const svg = f.readAll();

  auto const dpr = widget->devicePixelRatioF();
  for (auto const size : {16, 20, 22, 24, 32})
  {
    auto const dsize = static_cast<int>(size * dpr);

    icon.addPixmap(colorize(svg, dsize, size, dpr, normalColor),
                   QIcon::Normal);
    icon.addPixmap(colorize(svg, dsize, size, dpr, selectedColor),
                   QIcon::Selected);
    icon.addPixmap(colorize(svg, dsize, size, dpr, disabledColor),
                   QIcon::Disabled);
  }

  this->model.setRegisteredIcon(icon);
}

//-----------------------------------------------------------------------------
void GroundControlPointsViewPrivate::enableControls(
  bool state, bool haveLocation)
{
  this->UI.easting->setEnabled(state);
  this->UI.northing->setEnabled(state);
  this->UI.elevation->setEnabled(state);

  this->UI.actionDelete->setEnabled(state);
  this->UI.actionRevert->setEnabled(state);
  this->UI.actionCopyLocationLatLon->setEnabled(state && haveLocation);
  this->UI.actionCopyLocationLatLonElev->setEnabled(state && haveLocation);
  this->UI.actionCopyLocationLonLat->setEnabled(state && haveLocation);
  this->UI.actionCopyLocationLonLatElev->setEnabled(state && haveLocation);

  this->copyLocationButton->setEnabled(state);
}

//-----------------------------------------------------------------------------
void GroundControlPointsViewPrivate::showPoint(id_t id)
{
  if (this->helper && id != INVALID_POINT)
  {
    auto const& gcp = this->helper->groundControlPoint(id);
    if (gcp)
    {
      this->currentPoint = id;

      auto const& gl = gcp->geo_loc();
      auto const grl = [&gl]() -> kv::vector_3d {
        if (!gl.is_empty())
        {
          try
          {
            return gl.location(kv::SRID::lat_lon_WGS84);
          }
          catch (...)
          {
            qWarning() << "Geo-conversion from GCS" << gl.crs() << "failed";
          }
        }
        return { 0.0, 0.0, 0.0 };
      }();

      with_expr (qtScopedBlockSignals{this->UI.easting})
      {
        this->UI.easting->setValue(grl.x());
      }
      with_expr (qtScopedBlockSignals{this->UI.northing})
      {
        this->UI.northing->setValue(grl.y());
      }
      with_expr (qtScopedBlockSignals{this->UI.elevation})
      {
        this->UI.elevation->setValue(gcp->elevation());
      }

      this->enableControls(true, !gl.is_empty());

      return;
    }
  }

  this->currentPoint = INVALID_POINT;
  this->enableControls(false);
  return;
}

//-----------------------------------------------------------------------------
void GroundControlPointsViewPrivate::setPointPosition(id_t id)
{
  if (this->helper && id != INVALID_POINT)
  {
    auto const& gcp = this->helper->groundControlPoint(id);
    if (gcp)
    {
      auto const grl =
        kv::vector_2d{this->UI.easting->value(), this->UI.northing->value()};

      gcp->set_geo_loc({grl, kv::SRID::lat_lon_WGS84},
                       this->UI.elevation->value());
      gcp->set_geo_loc_user_provided(true);

      this->model.modifyPoint(id);
    }
  }
}

//-----------------------------------------------------------------------------
id_t GroundControlPointsViewPrivate::selectedPoint() const
{
  auto const& i = this->UI.pointsList->selectionModel()->currentIndex();
  auto const& ni = this->model.index(i.row(), 0, i.parent());
  auto const& id = this->model.data(ni, Qt::EditRole);
  return (id.isValid() ? id.value<id_t>() : INVALID_POINT);
}

//-----------------------------------------------------------------------------
void GroundControlPointsViewPrivate::copyLocation(
  bool northingFirst, bool includeElevation)
{
  auto const& gcp = this->helper->groundControlPoint(this->currentPoint);
  if (gcp)
  {
    auto const& gl = gcp->geo_loc();
    if (!gl.is_empty())
    {
      auto const grl = [&gl]() -> kv::vector_3d {
        try
        {
          return gl.location(kv::SRID::lat_lon_WGS84);
        }
        catch (...)
        {
          qWarning() << "Geo-conversion from GCS" << gl.crs() << "failed";
        }
        return { 0.0, 0.0, 0.0 };
      }();

      QStringList values;
      if (northingFirst)
      {
        values.append(QString::number(grl.y(), 'f', 9));
        values.append(QString::number(grl.x(), 'f', 9));
      }
      else
      {
        values.append(QString::number(grl.x(), 'f', 9));
        values.append(QString::number(grl.y(), 'f', 9));
      }
      if (includeElevation)
      {
        values.append(QString::number(gcp->elevation(), 'f', 3));
      }

      QApplication::clipboard()->setText(values.join(','));
    }
  }
}

//-----------------------------------------------------------------------------
GroundControlPointsView::GroundControlPointsView(
  QWidget* parent, Qt::WindowFlags flags)
  : QWidget{parent, flags}, d_ptr{new GroundControlPointsViewPrivate}
{
  QTE_D();

  // Set up UI
  d->UI.setupUi(this);
  d->AM.setupActions(d->UI, this);

  d->UI.pointsList->setModel(&d->model);
  d->UI.pointsList->setContextMenuPolicy(Qt::CustomContextMenu);

  connect(d->UI.pointsList->selectionModel(),
          &QItemSelectionModel::currentChanged,
          this, [d]{
            auto const id = d->selectedPoint();

            d->showPoint(id);

            if (d->helper)
            {
              d->helper->setActivePoint(id);
            }
          });

  d->updateRegisteredIcon(this);

  auto const clText = QStringLiteral("Copy Location");

  auto* const clMenu = new QMenu{clText, this};
  clMenu->addAction(d->UI.actionCopyLocationLatLon);
  clMenu->addAction(d->UI.actionCopyLocationLatLonElev);
  clMenu->addAction(d->UI.actionCopyLocationLonLat);
  clMenu->addAction(d->UI.actionCopyLocationLonLatElev);

  d->copyLocationButton = new QToolButton{d->UI.toolBar};
  d->copyLocationButton->setText(clText);
  d->copyLocationButton->setToolTip(clText);
  d->copyLocationButton->setIcon(
    qtUtil::standardActionIcon(QStringLiteral("copy-location")));
  d->copyLocationButton->setMenu(clMenu);
  d->copyLocationButton->setPopupMode(QToolButton::InstantPopup);

  auto* const spacer = new QWidget{d->UI.toolBar};
  spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

  d->UI.toolBar->insertWidget(d->UI.actionRevert, d->copyLocationButton);
  d->UI.toolBar->insertWidget(d->UI.actionRevert, spacer);

  d->popupMenu = new QMenu{this};
  d->popupMenu->addMenu(clMenu);
  d->popupMenu->addAction(d->UI.actionRevert);
  d->popupMenu->addAction(d->UI.actionDelete);

  connect(d->UI.pointsList, &QWidget::customContextMenuRequested,
          this, [d](QPoint const& pt){
            auto const i = d->UI.pointsList->indexAt(pt);
            if (i.isValid())
            {
              auto const gp = d->UI.pointsList->viewport()->mapToGlobal(pt);
              d->popupMenu->exec(gp);
            }
          });

  connect(d->UI.actionDelete, &QAction::triggered,
          this, [d]{
            if (d->helper && d->currentPoint != INVALID_POINT)
            {
              d->helper->removePoint(d->currentPoint);
            }
          });

  connect(d->UI.actionRevert, &QAction::triggered,
          this, [d]{
            if (d->helper && d->currentPoint != INVALID_POINT)
            {
              d->helper->resetPoint(d->currentPoint);
              d->model.modifyPoint(d->currentPoint);
            }
          });

  connect(d->UI.actionCopyLocationLatLon, &QAction::triggered,
          this, [d]{ d->copyLocation(true, false); });
  connect(d->UI.actionCopyLocationLatLonElev, &QAction::triggered,
          this, [d]{ d->copyLocation(true, true); });
  connect(d->UI.actionCopyLocationLonLat, &QAction::triggered,
          this, [d]{ d->copyLocation(false, false); });
  connect(d->UI.actionCopyLocationLonLatElev, &QAction::triggered,
          this, [d]{ d->copyLocation(false, true); });

  auto const dsvc = QOverload<double>::of(&QDoubleSpinBox::valueChanged);
  auto const spp = [d]{ d->setPointPosition(d->currentPoint); };
  connect(d->UI.easting, dsvc, this, spp);
  connect(d->UI.northing, dsvc, this, spp);
  connect(d->UI.elevation, dsvc, this, spp);

  d->enableControls(false);
}

//-----------------------------------------------------------------------------
GroundControlPointsView::~GroundControlPointsView()
{
}

//-----------------------------------------------------------------------------
void GroundControlPointsView::setHelper(GroundControlPointsHelper* helper)
{
  QTE_D();

  if (d->helper)
  {
    disconnect(d->helper, nullptr, this, nullptr);
    disconnect(d->helper, nullptr, &d->model, nullptr);
  }

  d->helper = helper;

  connect(helper, &GroundControlPointsHelper::pointChanged,
          this, [d](id_t id){
            if (d->currentPoint == id)
            {
              d->showPoint(id);
            }
          });
  connect(helper, &GroundControlPointsHelper::pointsRecomputed,
          this, [d](){
            if (d->currentPoint != INVALID_POINT)
            {
              d->showPoint(d->currentPoint);
            }
          });

  connect(helper, &GroundControlPointsHelper::activePointChanged,
          this, [d](id_t id){
            if (d->currentPoint != id)
            {
              constexpr auto flags =
                QItemSelectionModel::ClearAndSelect |
                QItemSelectionModel::Current | QItemSelectionModel::Rows;

              d->showPoint(id);

              auto const& index = d->model.find(id);
              d->UI.pointsList->selectionModel()->select(index, flags);
            }
          });

  connect(helper, &GroundControlPointsHelper::pointAdded,
          &d->model, &GroundControlPointsModel::addPoint);
  connect(helper, &GroundControlPointsHelper::pointRemoved,
          &d->model, &GroundControlPointsModel::removePoint);
  connect(helper, &GroundControlPointsHelper::pointsReloaded,
          &d->model, &GroundControlPointsModel::resetPoints);

  d->model.setPointData(helper->groundControlPoints());
}

//-----------------------------------------------------------------------------
void GroundControlPointsView::changeEvent(QEvent* e)
{
  if (e && e->type() == QEvent::PaletteChange)
  {
    QTE_D();
    d->updateRegisteredIcon(this);
  }

  QWidget::changeEvent(e);
}

//-----------------------------------------------------------------------------
void GroundControlPointsView::showEvent(QShowEvent* e)
{
  QTE_D();

  disconnect(d->screenChanged);
  d->screenChanged =
    connect(this->window()->windowHandle(), &QWindow::screenChanged,
            this, [d, this] { d->updateRegisteredIcon(this); });

  QWidget::showEvent(e);
}
