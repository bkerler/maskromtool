#include "maskromtool.h"
#include "maskromtool_autogen/include/ui_maskromtool.h"
#include "romlineitem.h"
#include "rombititem.h"
#include "rombitfix.h"
#include "romscene.h"
#include "romthresholddialog.h"
#include "aboutdialog.h"

//Decoders should be abstracted more, if we don't farm everything out to zorrom.
#include "romdecoderascii.h"
#include "romdecoderpython.h"
#include "romdecoderjson.h"
#include "romdecodermarc4.h"
#include "romdecoderphotograph.h"

//DRC rules.
#include "romrulecount.h"
#include "romruleduplicate.h"
#include "romrulesanity.h"
#include "romruleambiguous.h"


#include<iostream>

#include <QFileDialog>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QGraphicsItem>
#include <QImageReader>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtGlobal>
#include <QTextStream>
#include <QDebug>
#include <QMessageBox>
#include <QtMath>
#include <QProgressDialog>


using namespace std;


MaskRomTool::MaskRomTool(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MaskRomTool) {
    /* This only constructs the GUI.  See main.cpp
     * if you are looking for the CLI argument parsing.
     */

    ui->setupUi(this);
    scene=new RomScene();
    scene->maskRomTool=this;
    ui->graphicsView->setScene(scene);
    view=ui->graphicsView;
    violationDialog.setMaskRomTool(this);
    RomRuleViolation::bitSize=bitSize;

    //We might enable OpenGL here, after it stabilizes.
}

void MaskRomTool::on_actionOpenGL_triggered(){
    //Do the switch!
    enableOpenGL();
}

void MaskRomTool::enableOpenGL(unsigned int antialiasing){
    //Enabled OpenGL.  Maybe better performance?
    QOpenGLWidget *gl = new QOpenGLWidget();
    if(antialiasing>0){
        QSurfaceFormat format;
        format.setSamples(antialiasing);
        gl->setFormat(format);
    }
    view->setViewport(gl);

    //One way street.
    ui->actionOpenGL->setEnabled(false);
    ui->actionOpenGL->setChecked(true);
}

MaskRomTool::~MaskRomTool() {
    //ui->graphicsView->setScene(0);
    //delete scene;
    delete ui;
}


//Always call this to remove an item, to keep things clean.
void MaskRomTool::removeItem(QGraphicsItem* item){
    if(rows.contains((RomLineItem*) item)){
        rows.remove((RomLineItem*) item);
        alignmentdirty=true;
    }else if(cols.contains((RomLineItem*) item)){
        cols.remove((RomLineItem*) item);
        alignmentdirty=true;
    }else if(bits.contains((RomBitItem*) item)){
        bits.remove((RomBitItem*) item);
        alignmentdirty=true;
    }else if(violations.contains((RomRuleViolation*) item)){
        violations.remove((RomRuleViolation*) item);
        //Violations are commentary; they don't dirty the alignment.

        //Gotta remove the violation so that it isn't used after a free.
        violationDialog.removeViolation((RomRuleViolation*) item);
    }else if(bitfixes.contains((RomBitFix*) item)){
        bitfixes.remove((RomBitFix*) item);
        //Bit fixes change the value but not the alignment.
    }else{
        qDebug()<<"Asked to remove"<<item<<"but it can't be found.";
        return;
    }
    //Remove the item if it's a part of the scene.
    if(item->scene()==scene)
        scene->removeItem(item);
    //Leaking memory would be bad.
    delete item;
}

//Switch between viewing modes.
void MaskRomTool::nextMode(){
    //static int mode=0;

    setBitsVisible(!bitsVisible);
    setViolationsVisible(bitsVisible);
    ui->actionBits->setChecked(bitsVisible);
    ui->actionViolations->setChecked(bitsVisible);
}

void MaskRomTool::on_actionPhotograph_triggered(){
    static QBrush whitebrush(Qt::GlobalColor::white, Qt::SolidPattern);
    if(ui->actionPhotograph->isChecked())
        ui->graphicsView->setBackgroundBrush(background);
    else
        ui->graphicsView->setBackgroundBrush(whitebrush);
}
void MaskRomTool::on_actionRowsColumns_triggered(){
    setLinesVisible(ui->actionRowsColumns->isChecked());
}
void MaskRomTool::on_actionBits_triggered(){
    setBitsVisible(ui->actionBits->isChecked());
}
void MaskRomTool::on_actionViolations_triggered(){
    setViolationsVisible(ui->actionViolations->isChecked());
}
void MaskRomTool::on_actionViolationsDialog_triggered(){
    this->violationDialog.show();
}
void MaskRomTool::on_actionCrosshair_triggered(){
    scene->setCrosshairVisible(ui->actionCrosshair->isChecked());
}


void MaskRomTool::setLinesVisible(bool b){
    foreach (QGraphicsLineItem* item, rows){
        item->setVisible(b);
    }
    foreach (QGraphicsLineItem* item, cols){
        item->setVisible(b);
    }
}
void MaskRomTool::setBitsVisible(bool b){
    bitsVisible=b; //Default for new bits.
    foreach (QGraphicsItem* item, bits){
        item->setVisible(b);
    }
}
void MaskRomTool::setViolationsVisible(bool b){
    foreach (QGraphicsItem* item, violations){
        item->setVisible(b);
    }
}

//Highlights one item close to the center.
void MaskRomTool::centerOn(QGraphicsItem* item){
    view->centerOn(item);
}


//We might be a GUI, but keyboards are where it's add!
void MaskRomTool::keyPressEvent(QKeyEvent *event){
    QGraphicsTextItem *text;
    QGraphicsLineItem *line;

    RomLineItem *rlitem;
    static QLineF lastrow, lastcol;

    int key=event->key();

    switch(key){
    //Zoom/view keys.
    case Qt::Key_Q: //Reset Zoom
        ui->graphicsView->resetTransform();
        ui->graphicsView->totalScaleFactor=1;
        break;
    case Qt::Key_A:
        if(event->modifiers()&Qt::SHIFT){ //Damage/Ambiguate bit.
            damageBit(scene->scenepos);
        }else{ //Zoom in.
            ui->graphicsView->scale(1/1.2);
        }
        break;
    case Qt::Key_Z: //Zoom out.
        ui->graphicsView->scale(1.2);
        break;
    case Qt::Key_Tab: //Show/Hide BG
        nextMode();
        break;
    case Qt::Key_F:
        if(event->modifiers()&Qt::SHIFT){ //Force a Bit
            fixBit(scene->scenepos);
            statusBar()->showMessage(tr("Forced a bit."));
        }else{//Focus
            ui->graphicsView->centerOn(scene->focusItem());
            statusBar()->showMessage(tr("Focusing on selected item."));
        }
        break;
    case Qt::Key_V: //DRC
        runDRC(event->modifiers()&Qt::SHIFT);
        break;

    //Modify the focus object.
    case Qt::Key_D: //Delete an object.  With Shift, it deletes many.
        if(event->modifiers()&Qt::SHIFT){
            foreach(QGraphicsItem* item, scene->selection){
                if(rows.contains((RomLineItem*) item) || cols.contains((RomLineItem*) item))
                    removeItem(item);
            }
            scene->setFocusItem(0);
            statusBar()->showMessage(tr("Deleted all selected items."));
        }else if(scene->focusItem()){
            removeItem(scene->focusItem());
            scene->setFocusItem(0);
            statusBar()->showMessage(tr("Deleted item."));
        }else{
            statusBar()->showMessage(tr("There's no item to delete.  Maybe SHIFT+D?"));
        }
        markBits();
        break;

    case Qt::Key_S: //Set Position
        if(event->modifiers()&Qt::CTRL){
            on_saveButton_triggered();
        }else if(scene->focusItem()){
            scene->focusItem()->setPos(scene->scenepos);
            markBits();
        }
        break;


    //These insert an object and set its focus.
    case Qt::Key_Space:
    case Qt::Key_R: //Row line.
        rlitem=new RomLineItem(RomLineItem::LINEROW);
        if(event->key()==Qt::Key_Space
                || event->modifiers()&Qt::SHIFT)
            rlitem->setLine(lastrow);
        else
            rlitem->setLine(0, 0,
                        scene->presspos.x()-scene->scenepos.x(),
                        scene->presspos.y()-scene->scenepos.y()
                    );
        lastrow=rlitem->globalline();
        scene->addItem(rlitem);
        rlitem->setPos(scene->scenepos);
        scene->setFocusItem(rlitem);
        rows.insert(rlitem);
        markLine(rlitem);
        break;

    case Qt::Key_C: //Column line.
        rlitem=new RomLineItem(RomLineItem::LINECOL);
        if(event->modifiers()&Qt::SHIFT)
            rlitem->setLine(lastcol);
        else
            rlitem->setLine(0, 0,
                        scene->presspos.x()-scene->scenepos.x(),
                        scene->presspos.y()-scene->scenepos.y()
                    );
        lastcol=rlitem->globalline();
        scene->addItem(rlitem);
        rlitem->setPos(scene->scenepos);
        scene->setFocusItem(rlitem);
        cols.insert(rlitem);
        markLine(rlitem);
        break;

    //These operator on the loaded data.
    case Qt::Key_M:  //Mark Bits.
        markBits();
        if(asciiDialog.isVisible())
            on_asciiButton_triggered();
        statusBar()->showMessage(tr("Marked bits."));
        break;
    }
}

//Direction function to run the rules, called by CLI and GUI.
bool MaskRomTool::runDRC(bool all){
    //Clear the field for new violations.
    clearViolations();

    statusBar()->showMessage(tr("Running the Design Rule Checks. (DRC)"));

    RomRuleCount count;
    if(ui->drcCount->isChecked() || all)
        count.evaluate(this);

    RomRuleDuplicate dupes;
    if(ui->drcDuplicates->isChecked() || all)
        dupes.evaluate(this);

    RomRuleSanity sanity;
    if(ui->drcSanity->isChecked() || all)
        sanity.evaluate(this);

    RomRuleAmbiguous ambiguity;
    if(ui->drcAmbiguous->isChecked() || all)
        ambiguity.evaluate(this);


    statusBar()->showMessage(tr("Found %1 Design Rule Check (DRC) violations.").arg(violations.count()));
    if(violations.count()>0)
        on_actionViolationsDialog_triggered();

    return true;
}
//Menu item to run the rules.
void MaskRomTool::on_actionRunDRC_triggered(){
    runDRC();
}

//Marks a DRC violation in the scene.
void MaskRomTool::addViolation(RomRuleViolation* violation){
    QString message("%1: %2");
    qDebug()<<message.arg((violation->error?"ERROR":"WARNING"),
                              violation->title);

    violations.insert(violation);
    scene->addItem(violation);
    violationDialog.addViolation(violation);
}
//Clears all DRC violation.  Called before redrawing them.
void MaskRomTool::clearViolations(){
    violationDialog.clearViolations();
    foreach (RomRuleViolation* v, violations){
        removeItem(v);
    }
}


//Exports ASCII for ZORROM/Bitviewer.
void MaskRomTool::on_exportASCII_triggered(){
    RomDecoderAscii exporter;
    QString filename = QFileDialog::getSaveFileName(this,tr("Save Bits"),
                                                    "bits.txt",
                                                    tr("Textfiles (*.txt)"));
    if(!filename.isEmpty())
        exporter.writeFile(this,filename);
}

//Exports Python for custom scripts.
void MaskRomTool::on_exportPython_triggered(){
    RomDecoderPython exporter;
    QString filename = QFileDialog::getSaveFileName(this,tr("Save Python"),
                                                    "bits.py",
                                                    tr("Python (*.py)"));
    if(!filename.isEmpty())
        exporter.writeFile(this,filename);
}

//Exports just the bit values and location as JSON.
void MaskRomTool::on_exportJSONBits_triggered(){
    RomDecoderJson json;
    QString filename = QFileDialog::getSaveFileName(this,tr("Save JSON"), tr("bits.json"));
    if(!filename.isEmpty())
        json.writeFile(this, filename);
}


//Exports a .bin file from a MARC4 ROM.
void MaskRomTool::on_exportMARC4_triggered(){
    RomDecoderMarc4 marc4;
    QString filename = QFileDialog::getSaveFileName(this,tr("Save ROM"), tr("marc4rom.bin"));
    if(!filename.isEmpty())
        marc4.writeFile(this, filename);
}

//Exports a .png image from the current project.
void MaskRomTool::on_exportPhotograph_triggered(){
    RomDecoderPhotograph photo;
    QString filename = QFileDialog::getSaveFileName(this,tr("Save Photograph"), tr("romphoto.png"));
    if(!filename.isEmpty()){
        photo.writeFile(this, filename);
    }
}

//Pop up the about dialog.
void MaskRomTool::on_aboutButton_triggered(){
    aboutDialog about;
    about.exec();
}

//Display the ASCII preview window.
void MaskRomTool::on_asciiButton_triggered(){
    RomDecoderAscii exporter;
    asciiDialog.setText(exporter.preview(this));
    asciiDialog.show();
}

//Pop a dialog to apply the threshold.
void MaskRomTool::on_thresholdButton_triggered(){
    thresholdDialog.setMaskRomTool(this);
    thresholdDialog.show();
    updateThresholdHistogram();
}

//Updates the histogram, iff the window is visible.
void MaskRomTool::updateThresholdHistogram(){
    if(!thresholdDialog.isVisible())
        return;

    qreal reds[256], greens[256], blues[256];
    qreal skyhigh=0;//Maximum value.

    //Zero the score.
    for(int i=0; i<256; i++)
        reds[i]=greens[i]=blues[i]=0;

    //Count the bits of each color.
    foreach(RomBitItem* bit, bits){
        long pixel=bit->bitvalue_raw(background);
        int r=((pixel>>16)&0xFF);
        int g=((pixel>>8)&0xFF);
        int b=((pixel)&0xFF);
        reds[r]++;
        greens[g]++;
        blues[b]++;
    }

    QLineSeries *red = new QLineSeries();
    red->setName("Red");
    QLineSeries *green = new QLineSeries();
    green->setName("Green");
    QLineSeries *blue = new QLineSeries();
    blue->setName("Blue");
    for(int i=0; i<256; i++){
        red->append(i,reds[i]);
        green->append(i,greens[i]);
        blue->append(i,blues[i]);
        if(reds[i]>skyhigh) skyhigh=reds[i];
        if(greens[i]>skyhigh) skyhigh=greens[i];
        if(blues[i]>skyhigh) skyhigh=blues[i];
    }

    QLineSeries *redmark = new QLineSeries();
    redmark->append(thresholdR,0);
    redmark->append(thresholdR,skyhigh);
    QLineSeries *greenmark = new QLineSeries();
    greenmark->append(thresholdG,0);
    greenmark->append(thresholdG,skyhigh);
    QLineSeries *bluemark = new QLineSeries();
    bluemark->append(thresholdB,0);
    bluemark->append(thresholdB,skyhigh);


    histogramchart.legend()->hide();
    histogramchart.removeAllSeries();
    histogramchart.addSeries(red);
    histogramchart.addSeries(redmark);
    histogramchart.addSeries(green);
    histogramchart.addSeries(greenmark);
    histogramchart.addSeries(blue);
    histogramchart.addSeries(bluemark);

    //Colors must be set *after* adding them to the chart.
    red->setColor(Qt::red);
    redmark->setColor(Qt::red);
    green->setColor(Qt::green);
    greenmark->setColor(Qt::green);
    blue->setColor(Qt::blue);
    bluemark->setColor(Qt::blue);

    histogramchart.createDefaultAxes();
    histogramchart.setTitle("Bit Histogram");

    thresholdDialog.setChart(&histogramchart);
}


//Set the threshold.
void MaskRomTool::setBitThreshold(qreal r, qreal g, qreal b){
    thresholdR=r;
    thresholdG=g;
    thresholdB=b;

    //Redraws only if the histogram is visible.
    updateThresholdHistogram();
}

//Get the threshold.
void MaskRomTool::getBitThreshold(qreal &r, qreal &g, qreal &b){
    r=thresholdR;
    g=thresholdG;
    b=thresholdB;

    qDebug()<<"Returning RGB"<<r<<g<<b;
}

//Sets the bit display size.
void MaskRomTool::setBitSize(qreal size){
    //qDebug()<<"Changing bit size from"<<bitSize<<"to"<<size;
    bitSize=size;

    //We do not remark the bits here, but we do resize them.
    foreach (RomBitItem* bit, bits)
        bit->setBitSize(bitSize);
    foreach (RomBitFix* fix, bitfixes)
        fix->setBitSize(bitSize);

    //Violations are not dynamically resized.
    RomRuleViolation::bitSize=bitSize;
}
//Gets the bit display size.
qreal MaskRomTool::getBitSize(){
    qDebug()<<"Getting a bit size of "<<bitSize;
    return bitSize;
}

//Opens *either* an image or a JSON description of lines.
void MaskRomTool::fileOpen(QString filename){
    //On macOS, each window likes to have a file attached.
    window()->setWindowFilePath(filename);
    QProgressDialog progress("Loading", "Abort Load", 0, 10, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.show();
    progress.setValue(5);

    if(!filename.endsWith(".json")){
        imagefilename=filename;


        //This is necessary in QT6, but isn't defined in QT5.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        qDebug()<<"Allocation limit was "<<QImageReader::allocationLimit()<<"MB";
        qDebug()<<"Disabling allocation limit.";
        QImageReader::setAllocationLimit(0);
#endif

        //We load the image as a background, so it's not really a part of the scene.
        qDebug()<<"Loading background image: "<<imagefilename;
        background=QImage(imagefilename);
        if(background.isNull()){
            QMessageBox msgBox;
            msgBox.setText("Error opening "+imagefilename);
            msgBox.exec();
            return;
        }

        ui->graphicsView->setBackgroundBrush(background);
        //With the view the same as the image dimensions, we get fancy scrollbars and stuff.
        ui->graphicsView->setSceneRect(0,0,background.width(), background.height());

        qDebug()<<"Loaded background image of "
               <<background.width()<<","<<background.height();
        progress.close();
    }

    //And maybe there's a .json file to load?
    QFile file;
    if(!filename.endsWith(".json"))
        file.setFileName(filename+".json");
    else
        file.setFileName(filename);

    if(!file.open(QIODevice::ReadOnly)){
        qDebug() << "Json file couldn't be opened/found.  Starting from scratch.";
        return;
    }

    //Read the JSON bytes.
    QByteArray byteArray;
    byteArray = file.readAll();
    file.close();

    //Check the file format.
    QJsonParseError parseError;
    QJsonDocument jsonDoc;
    jsonDoc = QJsonDocument::fromJson(byteArray, &parseError);
    if(parseError.error != QJsonParseError::NoError){
        qWarning() << "Parse error at " << parseError.offset << ":" << parseError.errorString();
        //FIXME: Also a dialog box?
        return;
    }

    //Grab the object.
    importJSON(jsonDoc.object());
}

//Opens an image as the backgound for annotation, and matching .json if it's available.
void MaskRomTool::on_openButton_triggered(){
    QString newfilename = QFileDialog::getOpenFileName(this,tr("Open Image"), tr("Image Files (*.png *.jpg *.bmp)"));
    if(newfilename.isEmpty())
        return;

    fileOpen(newfilename);
}

//A line item has both a start location and a vector.  This normalizes it to only be a line.
QLineF MaskRomTool::lineFromLineItem(QGraphicsLineItem *a){
    QLineF linea=a->line();

    qreal x1, y1, x2, y2;
    x1=linea.x1()+a->x();
    y1=linea.y1()+a->y();
    x2=linea.x2()+a->x();
    y2=linea.y2()+a->y();

    return QLineF(x1, y1, x2, y2);
}

//Returns the collision point of two lines, if one exists.
QPointF MaskRomTool::bitLocation(QGraphicsLineItem *a, QGraphicsLineItem *b){
    QPointF toret;
    QLineF::IntersectionType type;
    lineFromLineItem(a).intersects(lineFromLineItem(b), &toret);

    return toret;
}

//Is there a bit at a point?
RomBitItem* MaskRomTool::getBit(QPointF point){
    QList<QGraphicsItem*> items=scene->items(point);
    RomBitItem* bit=0;
    foreach (QGraphicsItem* item, items){
        if(bits.contains((RomBitItem*) item))
            bit=(RomBitItem*) item;
    }
    return bit;
}

//Given a bit or its position, is there a matching BitFix?
RomBitFix* MaskRomTool::getBitFix(QPointF point, bool create){
    return getBitFix(getBit(point), create);
}
RomBitFix* MaskRomTool::getBitFix(RomBitItem* bit, bool create){
    if(!bit)
        return 0;

    QList<QGraphicsItem*> items=scene->items(bit->pos());
    RomBitFix* fix=0;
    foreach (QGraphicsItem* item, items){
        if(bitfixes.contains((RomBitFix*) item))
            fix=(RomBitFix*) item;
    }
    if(create && !fix){
        fix=new RomBitFix(bit);
        scene->addItem(fix);
        bitfixes.insert(fix);
    }
    return fix;
}
//Fixes a bit at a position.
void MaskRomTool::fixBit(QPointF point){
    if(!bitsVisible) return;
    //We fix the bit's position, not the click position.
    RomBitItem* bit=getBit(point);
    fixBit(bit);
}
void MaskRomTool::fixBit(RomBitItem* bit){
    if(!bitsVisible) return;
    RomBitFix* fix=getBitFix(bit, true);
    if(fix){
        fix->setValue(!fix->bitValue());
        bit->setFix(fix);
    }
}
//Marks a bit at a position as damaged.
void MaskRomTool::damageBit(QPointF point){
    if(!bitsVisible) return;
    //We fix the bit's position, not the click position.
    RomBitItem* bit=getBit(point);
    damageBit(bit);
}
void MaskRomTool::damageBit(RomBitItem* bit){
    if(!bitsVisible) return;
    RomBitFix* fix=getBitFix(bit, true);
    if(fix){
        fix->setAmbiguious(!fix->bitAmbiguous());
        bit->setFix(fix);
    }
}

//Saves the lines for later import.
void MaskRomTool::on_saveButton_triggered(){
    QJsonObject obj=exportJSON();
    QByteArray ba = QJsonDocument(obj).toJson();

    statusBar()->showMessage(tr("Saved to ")+imagefilename+".json");

    QFile fout(imagefilename+".json");
    fout.open(QIODevice::WriteOnly);
    fout.write(ba);
}



//Marks an individual bit.
void MaskRomTool::markBit(QPointF point){
    alignmentdirty=true;

    //This rectangle will be a small box around the pixel.
    RomBitItem *bit=new RomBitItem(point, bitSize);
    scene->addItem(bit);
    //bit->setRect(point.x()-5, point.y()-5, 10, 10);
    bit->setVisible(bitsVisible);
    bits.insert(bit);

    bit->bitvalue_raw(background);
    bit->bitvalue_sample(background, thresholdR, thresholdG, thresholdB);

    bitcount++;
}

//Marks all of the bits that are on a line.
void MaskRomTool::markLine(RomLineItem* line){
    /* This is called in a loop to identify all of the lines,
     * but as a performance hack, we also use it mark the newly
     * created bits when a new row or column is placed.
     *
     * This is why we need to handle both rows and columns,
     * rather than just one type.
     */

    foreach(QGraphicsItem* item, scene->collidingItems(line)){
        //We are only looking for columns that collide with rows here.  All other collisions are irrelevant.
        if(line->linetype==RomLineItem::LINEROW && cols.contains((RomLineItem*) item)){
            RomLineItem* col=(RomLineItem*) item;
            QPointF point=bitLocation(line, col);
            markBit(point);
        }else if(line->linetype==RomLineItem::LINECOL && rows.contains((RomLineItem*) item)){
            RomLineItem* row=(RomLineItem*) item;
            QPointF point=bitLocation(row, line);
            markBit(point);
        }
    }

    //We also adjust the crosshairs to the last lines made.
    switch(line->linetype){
    case RomLineItem::LINEROW:
        scene->setRowAngle(line->line().angle());
        break;
    case RomLineItem::LINECOL:
        scene->setColAngle(line->line().angle());
        break;
    }
}

//Mark up all of the bits where rows and columns collide.
void MaskRomTool::markBits(){
    static long lastbitcount=50000;
    bool bitswerevisible=bitsVisible;

    //We're blowing away the alignment here, will need to rebuild it later.
    alignmentdirty=true;

    //First we remove all the old bits.
    foreach (QGraphicsItem* item, bits){
        removeItem(item);
    }

    bitcount=0;
    setBitsVisible(false);

    //Mark every line collision.
    foreach (RomLineItem* line, cols)
        markLine(line);

    //Mark all the fixes.
    markFixes();

    //Show the bits if--and only if--we've set that style.
    setBitsVisible(bitswerevisible);


    qDebug()<<"Marked"<<bitcount<<"bits.";
    lastbitcount=bitcount;
}

//Marks the bit fixes.
void MaskRomTool::markFixes(){
    /* We assume that fixes are far outnumbered by bits,
     * so it's far faster to search for every bit that
     * overlaps a fix than every fix that overlaps a bit.
     */
    bool bitswerevisible=bitsVisible;
    setBitsVisible(true);
    foreach (RomBitFix* fix, bitfixes){
        RomBitItem* bit=getBit(fix->pos());
        if(bit) // Apply the fix.
            bit->setFix(fix);
        else    // Destroy fixes that don't match bits.
            removeItem(fix);
    }
    setBitsVisible(bitswerevisible);
}

//Marks up all of the known bits with new samples.
void MaskRomTool::remarkBits(){
    foreach (RomBitItem* bit, bits){
        bit->bitvalue_raw(background);
        bit->bitvalue_sample(background, thresholdR, thresholdG, thresholdB);
    }
    //We remark all fixes here, because it's fast.
    markFixes();
}


//Marks the table of bits.  This is needed for export, but not to update the display.
RomBitItem* MaskRomTool::markBitTable(){
    static RomBitItem* firstbit=0;

    //Make sure all the bits are ready.
    if(alignmentdirty){
        markBits();
        if(!aligner)
            aligner=new RomAlignerDefault();
        alignmentdirty=false;
        firstbit=aligner->markBitTable(bits);
    }

    //If the alignment was cancelled, it's dirty.
    if(!firstbit)
        alignmentdirty=true;

    return firstbit;
}

//This exports the state to JSON.
QJsonObject MaskRomTool::exportJSON(){
    QJsonObject root;
    //Could be handy if anyone needs to reverse engineer this.
    root["00about"]="Export from MaskROMTool by Travis Goodspeed";
    root["00github"]="http://github.com/travisgoodspeed/mcuexploits/";
    root["00imagefilename"]=imagefilename;

    /* We try not to break compatibility, but as features are added,
     * we must add new
     */
    root["00version"]="2022.09.28";

    //These threshold values will change in a later version.
    QJsonObject settings;
    settings["red"]=thresholdR;
    settings["green"]=thresholdG;
    settings["blue"]=thresholdB;
    settings["bitsize"]=bitSize;
    root["settings"]=settings;

    QJsonArray jrows;
    foreach (RomLineItem* item, rows){
        QJsonObject o;
        item->write(o);
        jrows.push_back(o);
    }
    root["rows"] = jrows;

    QJsonArray jcols;
    foreach (RomLineItem* item, cols){
        QJsonObject o;
        item->write(o);
        jcols.push_back(o);
    }
    root["cols"] = jcols;

    QJsonArray jfixes;
    foreach (RomBitFix* item, bitfixes){
        QJsonObject o;
        item->write(o);
        jfixes.push_back(o);
    }
    root["bitfixes"] = jfixes;

    /* For now, we don't save the bits but instead regenerate
       them from the rows, cols, and thresholds.  This might cause
       some bit errors as code refactors, but hopefully fixed points
       will take care of that.
     */
    return root;
}

//The imports the state from JSON.
void MaskRomTool::importJSON(QJsonObject o){
    QJsonValue github=o.value("00github");
    QJsonValue about=o.value("00about");

    //Thresholds default to zero when undefined.
    QJsonObject settings=o["settings"].toObject();
    QJsonValue red=settings.value("red");
    QJsonValue green=settings.value("green");
    QJsonValue blue=settings.value("blue");
    setBitThreshold(red.toDouble(0), green.toDouble(0), blue.toDouble(0));
    setBitSize(settings.value("bitsize").toDouble(10));


    //Line items.
    QJsonArray jrows=o["rows"].toArray();
    QJsonArray jcols=o["cols"].toArray();
    //Fixes
    QJsonArray jfixes=o["bitfixes"].toArray();

    //Progress bars are cool when things get slow.
    QProgressDialog progress("Loading JSON", "Abort Load", 0, jrows.size()+jcols.size()+jfixes.size(), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.show();
    int count=0;

    //Rows.
    for(int i = 0; i < jrows.size(); i++){
        RomLineItem *l=new RomLineItem(RomLineItem::LINEROW);
        l->read(jrows[i]);
        scene->addItem(l);
        rows.insert(l);

        progress.setValue(count++);
    }

    //Columns.
    for(int i = 0; i < jcols.size(); i++){
        RomLineItem *c=new RomLineItem(RomLineItem::LINECOL);
        c->read(jcols[i]);
        scene->addItem(c);
        cols.insert(c);

        progress.setValue(count++);
    }

    //Bit Fixes
    for(int i = 0; i < jfixes.size(); i++){
        RomBitFix *fix=new RomBitFix(0);
        fix->read(jfixes[i]);
        scene->addItem(fix);
        bitfixes.insert(fix);

        progress.setValue(count++);
    }


    qDebug()<<"Done loading, now marking bits.";

    progress.close();

    //After loading all that, we should be able to decode the bits.
    markBits();
    setBitSize(bitSize);
}




