/*
 * Stellarium
 * Copyright (C) 2006 Fabien Chereau
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA  02110-1335, USA.
 */

#include "StelApp.hpp"
#include "StelSRGB.hpp"
#include "Dithering.hpp"
#include "StelCore.hpp"
#include "StelSkyDrawer.hpp"
#include "StelPhotoBloom.hpp"
#include "StelPhotoMeter.hpp"
#include "StelMainView.hpp"
#include "StelSplashScreen.hpp"
#include "StelUtils.hpp"
#include "StelTextureMgr.hpp"
#include "StelObjectMgr.hpp"
#include "ConstellationMgr.hpp"
#include "AsterismMgr.hpp"
#include "HipsMgr.hpp"
#include "NebulaMgr.hpp"
#include "LandscapeMgr.hpp"
#include "LightDomes.hpp"
#include "AuroraMgr.hpp"
#include "CustomObjectMgr.hpp"
#include "HighlightMgr.hpp"
#include "GridLinesMgr.hpp"
#include "MilkyWay.hpp"
#include "ZodiacalLight.hpp"
#include "LabelMgr.hpp"
#include "MarkerMgr.hpp"
#include "SolarSystem.hpp"
#include "NomenclatureMgr.hpp"
#include "SporadicMeteorMgr.hpp"
#include "SpecificTimeMgr.hpp"
#include "StarMgr.hpp"
#include "StelProjector.hpp"
#include "StelLocationMgr.hpp"
#include "ToastMgr.hpp"
#include "StelActionMgr.hpp"
#include "StelPropertyMgr.hpp"
#include "StelProgressController.hpp"
#include "StelModuleMgr.hpp"
#include "StelLocaleMgr.hpp"
#include "StelSkyCultureMgr.hpp"
#include "StelFileMgr.hpp"
#include "StelSkyLayerMgr.hpp"
#include "StelAudioMgr.hpp"
#include "StelVideoMgr.hpp"
#include "StelSpeechMgr.hpp"
#include "SpecialMarkersMgr.hpp"
#include "StelViewportEffect.hpp"
#include "StelGuiBase.hpp"
#include "StelGui.hpp"
#include "StelGuiItems.hpp"
#include "StelPainter.hpp"
#ifdef ENABLE_SCRIPTING
 #include "StelScriptMgr.hpp"
 #include "StelMainScriptAPIProxy.hpp"

#ifdef USE_STATIC_PLUGIN_CALENDARS
 #include "../plugins/Calendars/src/Calendars.hpp"
#endif
#endif


#include <cstdlib>
#include <iostream>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLFramebufferObject>
#include <QString>
#include <QStringList>
#include <QSysInfo>
#include <QTextStream>
#include <QTimer>
#include <QDir>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QDateTime>
#include <QGenericMatrix>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QFontDatabase>
#if !QT_CONFIG(opengles2)
# include <QOpenGLFunctions_3_3_Core>
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QImageReader>
#endif
#ifdef ENABLE_SPOUT
#include <QMessageBox>
#include "SpoutSender.hpp"
#endif

#ifdef USE_STATIC_PLUGIN_HELLOSTELMODULE
Q_IMPORT_PLUGIN(HelloStelModuleStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_SIMPLEDRAWLINE
Q_IMPORT_PLUGIN(SimpleDrawLineStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_ANGLEMEASURE
Q_IMPORT_PLUGIN(AngleMeasureStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_ARCHAEOLINES
Q_IMPORT_PLUGIN(ArchaeoLinesStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_CALENDARS
Q_IMPORT_PLUGIN(CalendarsStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_SATELLITES
Q_IMPORT_PLUGIN(SatellitesStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_TEXTUSERINTERFACE
Q_IMPORT_PLUGIN(TextUserInterfaceStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_OCULARS
Q_IMPORT_PLUGIN(OcularsStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_OCULUS
Q_IMPORT_PLUGIN(OculusStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_TELESCOPECONTROL
Q_IMPORT_PLUGIN(TelescopeControlStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_SOLARSYSTEMEDITOR
Q_IMPORT_PLUGIN(SolarSystemEditorStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_LENSDISTORTIONESTIMATOR
Q_IMPORT_PLUGIN(LensDistortionEstimatorStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_METEORSHOWERS
Q_IMPORT_PLUGIN(MeteorShowersStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_NAVSTARS
Q_IMPORT_PLUGIN(NavStarsStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_NOVAE
Q_IMPORT_PLUGIN(NovaeStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_SUPERNOVAE
Q_IMPORT_PLUGIN(SupernovaeStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_QUASARS
Q_IMPORT_PLUGIN(QuasarsStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_PULSARS
Q_IMPORT_PLUGIN(PulsarsStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_EXOPLANETS
Q_IMPORT_PLUGIN(ExoplanetsStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_EQUATIONOFTIME
Q_IMPORT_PLUGIN(EquationOfTimeStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_POINTERCOORDINATES
Q_IMPORT_PLUGIN(PointerCoordinatesStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_OBSERVABILITY
Q_IMPORT_PLUGIN(ObservabilityStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_SCENERY3D
Q_IMPORT_PLUGIN(Scenery3dStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_SKYCULTUREMAKER
Q_IMPORT_PLUGIN(SkyCultureMakerStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_REMOTECONTROL
Q_IMPORT_PLUGIN(RemoteControlStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_REMOTESYNC
Q_IMPORT_PLUGIN(RemoteSyncStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_VTS
Q_IMPORT_PLUGIN(VtsStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_ONLINEQUERIES
Q_IMPORT_PLUGIN(OnlineQueriesPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_PLANES
Q_IMPORT_PLUGIN(PlanesStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_NEBULATEXTURES
Q_IMPORT_PLUGIN(NebulaTexturesStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_MOSAICCAMERA
Q_IMPORT_PLUGIN(MosaicCameraStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_TIMENAVIGATOR
Q_IMPORT_PLUGIN(TimeNavigatorStelPluginInterface)
#endif

#ifdef USE_STATIC_PLUGIN_OBJECTVISIBILITY
Q_IMPORT_PLUGIN(ObjectVisibilityStelPluginInterface)
#endif

// Initialize static variables
StelApp* StelApp::singleton = nullptr;
qint64 StelApp::startMSecs = 0;
double StelApp::animationScale = 1.;

void StelApp::initStatic()
{
	StelApp::startMSecs = QDateTime::currentMSecsSinceEpoch();
}

void StelApp::deinitStatic()
{
	StelApp::startMSecs = 0;
}

/*************************************************************************
 Create and initialize the main Stellarium application.
*************************************************************************/
StelApp::StelApp(StelMainView *parent)
	: QObject(parent)
	, randomGenerator(nullptr)
	, mainWin(parent)
	, core(nullptr)
	, moduleMgr(nullptr)
	, localeMgr(nullptr)
	, skyCultureMgr(nullptr)
	, actionMgr(nullptr)
	, propMgr(nullptr)
	, textureMgr(nullptr)
	, stelObjectMgr(nullptr)
	, planetLocationMgr(nullptr)
	, networkAccessManager(nullptr)
	, audioMgr(nullptr)
	, videoMgr(nullptr)
	, speechMgr(nullptr)
	, skyImageMgr(nullptr)
#ifdef ENABLE_SCRIPTING
	, scriptAPIProxy(nullptr)
	, scriptMgr(nullptr)
#endif
	, stelGui(nullptr)
	, devicePixelsPerPixel(1.)
	, fps(0)
	, frame(0)
	, frameTimeAccum(0.)
	, flagNightVision(false)
	, confSettings(nullptr)
	, initialized(false)
	, saveProjW(-1.)
	, saveProjH(-1.)
	, nbDownloadedFiles(0)
	, totalDownloadedSize(0)
	, nbUsedCache(0)
	, totalUsedCacheSize(0)
	, screenFontSize(getDefaultGuiFontSize())
	, renderBuffer(nullptr)
	, viewportEffect(nullptr)
	, gl(nullptr)
	, flagUseDecDegreesCoords(false)
	, flagUseDecDegreesOther(false)
	, flagUseAzimuthFromSouth(false)
	, flagUseNegativeHourAngles(false)
	, flagUsePolarDistance(false)
	, flagUseFormattingOutput(false)
	, flagUseCCSDesignation(false)
	, flagOverwriteInfoColor(false)
	, overwriteInfoColor(Vec3f(1.f))
	, daylightInfoColor(Vec3f(0.f))
	, flagImmediateSave(false)
	#ifdef ENABLE_SPOUT
	, spoutSender(nullptr)
	#endif
	, currentFbo(0)
{
	setObjectName("StelApp");

	// Can't create 2 StelApp instances
	Q_ASSERT(!singleton);
	singleton = this;

	moduleMgr = new StelModuleMgr();
	randomGenerator = new QRandomGenerator(static_cast<quint32>(QDateTime::currentMSecsSinceEpoch()));
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	// QImageReader has a new limitation. For super-large landscapes or other textures, we need to circumvent this.
	QImageReader::setAllocationLimit(1024); // Allow max texture size 16k x 16k
#endif
}

/*************************************************************************
 Deinitialize and destroy the main Stellarium application.
*************************************************************************/
StelApp::~StelApp()
{
	qInfo() << qPrintable(QString("Downloaded %1 files (%2 kbytes) in a session of %3 sec (average of %4 kB/s + %5 files from cache (%6 kB)).").arg(nbDownloadedFiles).arg(totalDownloadedSize/1024).arg(getTotalRunTime()).arg(static_cast<double>(totalDownloadedSize/1024)/getTotalRunTime()).arg(nbUsedCache).arg(totalUsedCacheSize/1024));

	stelObjectMgr->unSelect();
	moduleMgr->unloadModule("StelVideoMgr", false);  // We need to delete it afterward
	moduleMgr->unloadModule("StelSkyLayerMgr", false);  // We need to delete it afterward
	moduleMgr->unloadModule("StelObjectMgr", false);// We need to delete it afterward
	moduleMgr->unloadModule("StelSkyCultureMgr", true);// No need to delete only later!
	StelModuleMgr* tmp = moduleMgr;
	moduleMgr = new StelModuleMgr(); // Create a secondary instance to avoid crashes at other deinit
	delete tmp; tmp=nullptr;
	delete skyImageMgr; skyImageMgr=nullptr;
	delete core; core=nullptr;
	delete localeMgr; localeMgr=nullptr;
	delete audioMgr; audioMgr=nullptr;
	delete videoMgr; videoMgr=nullptr;
	//delete speechMgr; speechMgr=nullptr;
	delete stelObjectMgr; stelObjectMgr=nullptr; // Delete the module by hand afterward
	delete textureMgr; textureMgr=nullptr;
	delete planetLocationMgr; planetLocationMgr=nullptr;
	delete moduleMgr; moduleMgr=nullptr; // Delete the secondary instance
	delete actionMgr; actionMgr = nullptr;
	delete propMgr; propMgr = nullptr;
	delete renderBuffer; renderBuffer = nullptr;
	delete randomGenerator; randomGenerator=nullptr;
	Q_ASSERT(singleton);
	singleton = nullptr;
}

void StelApp::setupNetworkProxy()
{
	QString proxyHost = confSettings->value("proxy/host_name").toString();
	QString proxyPort = confSettings->value("proxy/port").toString();
	QString proxyUser = confSettings->value("proxy/user").toString();
	QString proxyPass = confSettings->value("proxy/password").toString();
	QString proxyType = confSettings->value("proxy/type").toString();

	// If proxy settings not found in config, use environment variable
	// if it is defined.  (Config file over-rides environment).
	if (proxyHost.isEmpty() && proxyUser.isEmpty() && proxyPass.isEmpty() && proxyPort.isEmpty())
	{
		char *httpProxyEnv;
		httpProxyEnv = std::getenv("http_proxy");
		if (!httpProxyEnv)
		{
			httpProxyEnv = std::getenv("HTTP_PROXY");
		}
		if (httpProxyEnv)
		{
			QString proxyString = QString(httpProxyEnv);
			if (!proxyString.isEmpty())
			{
				// Handle http_proxy of the form
				// proto://username:password@fqdn:port
				// e.g.:
				// http://usr:pass@proxy.loc:3128/
				// http://proxy.loc:3128/
				// http://2001:62a:4:203:6ab5:99ff:fef2:560b:3128/
				// http://foo:bar@2001:62a:4:203:6ab5:99ff:fef2:560b:3128/
				static const QRegularExpression pre("^([^:]+://)?(?:([^:]+):([^@]*)@)?(.+):([\\d]+)");
				QRegularExpressionMatch preMatch=pre.match(proxyString);
				if (proxyString.indexOf(pre) >= 0)
				{
					proxyType = preMatch.captured(1);
					proxyUser = preMatch.captured(2);
					proxyPass = preMatch.captured(3);
					proxyHost = preMatch.captured(4);
					proxyPort = preMatch.captured(5);
				}
				else
				{
					qDebug() << "indecipherable environment variable http_proxy:" << proxyString;
					return;
				}
			}
		}
	}

	bool useSocksProxy = proxyType.contains("socks", Qt::CaseInsensitive);

	if (!proxyHost.isEmpty())
	{
		QNetworkProxy proxy;
		if (useSocksProxy)
			proxy.setType(QNetworkProxy::Socks5Proxy);
		else
			proxy.setType(QNetworkProxy::HttpProxy);
		proxy.setHostName(proxyHost);
		if (!proxyPort.isEmpty())
			proxy.setPort(proxyPort.toUShort());

		if (!proxyUser.isEmpty())
			proxy.setUser(proxyUser);

		if (!proxyPass.isEmpty())
			proxy.setPassword(proxyPass);

		QString ppDisp = proxyPass;
		ppDisp.fill('*');
		if (useSocksProxy)
			qDebug() << "Using SOCKS proxy:" << proxyUser << ppDisp << proxyHost << proxyPort;
		else
			qDebug() << "Using HTTP proxy:" << proxyUser << ppDisp << proxyHost << proxyPort;
		QNetworkProxy::setApplicationProxy(proxy);
	}
}

#ifdef ENABLE_SCRIPTING
void StelApp::initScriptMgr()
{
	scriptMgr->addModules();

#ifdef USE_STATIC_PLUGIN_CALENDARS
	Calendars *cal=GETSTELMODULE_SILENT(Calendars);
	if (cal)
		cal->makeCalendarsScriptable(scriptMgr);
#endif
	QString startupScript;
	if (qApp->property("onetime_startup_script").isValid())
		startupScript = qApp->property("onetime_startup_script").toString();
	else
		startupScript = confSettings->value("scripts/startup_script", "startup.ssc").toString();
	// Use a queued slot call to start the script only once the main qApp event loop is running...
	QMetaObject::invokeMethod(scriptMgr,
				  "runScript",
				  Qt::QueuedConnection,
				  Q_ARG(QString, startupScript));
}
#else
void StelApp::initScriptMgr() {}
#endif

QStringList StelApp::getCommandlineArguments()
{
	return qApp->property("stelCommandLine").toStringList();
}

void StelApp::init(QSettings* conf)
{
	gl = QOpenGLContext::currentContext()->functions();
	confSettings = conf;

	devicePixelsPerPixel = StelMainView::getInstance().devicePixelRatioF();
	qInfo() << "Initial high-DPI scaling factor:" << devicePixelsPerPixel;

	setScreenFontSize(confSettings->value("gui/screen_font_size", getDefaultGuiFontSize()).toInt());
	setGuiFontSize(confSettings->value("gui/gui_font_size", getDefaultGuiFontSize()).toInt());
	setScreenButtonScale(confSettings->value("gui/screen_button_scale", 100).toDouble());

	SplashScreen::present(guiFontSizeRatio());

	setFlagImmediateSave(confSettings->value("gui/immediate_save_details", false).toBool());

	core = new StelCore();
	if (!fuzzyEquals(saveProjW, -1.) && !fuzzyEquals(saveProjH, -1.))
		core->windowHasBeenResized(0, 0, saveProjW, saveProjH);
	
	//Initializing locale at the beginning to show all strings translated
	localeMgr = new StelLocaleMgr();
	localeMgr->init();
	//SplashScreen::showMessage(q_("Initializing locales..."));

	SplashScreen::showMessage(q_("Initializing textures..."));
	// Initialize AFTER creation of openGL context
	textureMgr = new StelTextureMgr();

	SplashScreen::showMessage(q_("Initializing network access..."));
	networkAccessManager = new QNetworkAccessManager(this);
	networkAccessManager->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
	SplashScreen::showMessage(q_("Initializing network disk cache..."));
	// Activate http cache if Qt version >= 4.5
	QNetworkDiskCache* cache = new QNetworkDiskCache(networkAccessManager);
	//make maximum cache size configurable (in MB)
	//the default Qt value (50 MB) is quite low, especially for DSS
	cache->setMaximumCacheSize(confSettings->value("main/network_cache_size",300).toInt() * 1024 * 1024);
	QString cachePath = StelFileMgr::getCacheDir();

	qInfo().noquote() << "Cache directory:" << QDir::toNativeSeparators(cachePath);
	cache->setCacheDirectory(cachePath);
	networkAccessManager->setCache(cache);
	connect(networkAccessManager, &QNetworkAccessManager::finished, this, &StelApp::reportFileDownloadFinished);

	// Proxy Initialisation
	SplashScreen::showMessage(q_("Initializing network proxy..."));
	setupNetworkProxy();

	//create non-StelModule managers
	propMgr = new StelPropertyMgr();
	skyCultureMgr = new StelSkyCultureMgr();
	getModuleMgr().registerModule(skyCultureMgr);

	planetLocationMgr = new StelLocationMgr();
	actionMgr = new StelActionMgr();

	// register non-modules for StelProperty tracking
	propMgr->registerObject(this);
	propMgr->registerObject(mainWin);

	// Stel Object Data Base manager
	SplashScreen::showMessage(q_("Initializing Object Database..."));
	stelObjectMgr = new StelObjectMgr();
	stelObjectMgr->init();
	getModuleMgr().registerModule(stelObjectMgr);	

	// Hips surveys
	SplashScreen::showMessage(q_("Initializing HiPS survey..."));
	HipsMgr* hipsMgr = new HipsMgr();
	hipsMgr->init();
	getModuleMgr().registerModule(hipsMgr);

	// Init the solar system first
	SplashScreen::showMessage(q_("Initializing Solar System objects..."));
	SolarSystem* ssystem = new SolarSystem();
	ssystem->init();
	getModuleMgr().registerModule(ssystem);

	// Load stars & their names
	SplashScreen::showMessage(q_("Initializing stars..."));
	StarMgr* hip_stars = new StarMgr();
	hip_stars->init();
	getModuleMgr().registerModule(hip_stars);

	SplashScreen::showMessage(q_("Initializing core..."));
	core->init();

	// Init the nomenclature for Solar system bodies
	SplashScreen::showMessage(q_("Initializing planetary nomenclature..."));
	NomenclatureMgr* nomenclature = new NomenclatureMgr();
	nomenclature->init();
	getModuleMgr().registerModule(nomenclature);

	// Init nebulas
	SplashScreen::showMessage(q_("Initializing deep-sky objects..."));
	NebulaMgr* nebulas = new NebulaMgr();
	nebulas->init();
	getModuleMgr().registerModule(nebulas);

	// Init milky way
	SplashScreen::showMessage(q_("Initializing Milky Way..."));
	MilkyWay* milky_way = new MilkyWay();
	milky_way->init();
	getModuleMgr().registerModule(milky_way);

	// Init zodiacal light
	SplashScreen::showMessage(q_("Initializing zodiacal light..."));
	ZodiacalLight* zodiacal_light = new ZodiacalLight();
	zodiacal_light->init();
	getModuleMgr().registerModule(zodiacal_light);

	// Init sky image manager
	SplashScreen::showMessage(q_("Initializing sky image layer..."));
	skyImageMgr = new StelSkyLayerMgr();
	skyImageMgr->init();
	getModuleMgr().registerModule(skyImageMgr);

	// Toast surveys
	SplashScreen::showMessage(q_("Initializing TOAST surveys..."));
	ToastMgr* toasts = new ToastMgr();
	toasts->init();
	getModuleMgr().registerModule(toasts);

	// Init audio manager. When media support is disabled,
	// we silently create dummy managers.
#ifdef ENABLE_MEDIA
	const bool audioOK = !(qApp->property("onetime_no-audio").toBool())
			&& confSettings->value("audio/enabled", true).toBool();
#else
	const bool audioOK = false;
#endif
	if (audioOK)
		SplashScreen::showMessage(q_("Initializing audio..."));
	audioMgr = new StelAudioMgr(audioOK);
	// QtMultimedia can create and destroy intermediate contexts during initialization,
	// displacing our main context, see GH#4143. So restore our context.
	ensureGLContextCurrent();

	// Init video manager
#ifdef ENABLE_MEDIA
	SplashScreen::showMessage(q_("Initializing video..."));
#endif
	videoMgr = new StelVideoMgr(audioOK);
	videoMgr->init();
	getModuleMgr().registerModule(videoMgr);

	// Init speech manager. If no media or Qt too low, this installs a dummy object, the initialisation of which need not be announced.
#if defined(ENABLE_MEDIA) && (QT_VERSION>=QT_VERSION_CHECK(6,6,0))
	SplashScreen::showMessage(q_("Initializing speech output..."));
#endif
	speechMgr = new StelSpeechMgr();
	speechMgr->init();
	getModuleMgr().registerModule(speechMgr);

	// Constellations
	SplashScreen::showMessage(q_("Initializing constellations..."));
	ConstellationMgr* constellations = new ConstellationMgr(hip_stars);
	constellations->init();
	getModuleMgr().registerModule(constellations);

	// Asterisms
	SplashScreen::showMessage(q_("Initializing asterisms..."));
	AsterismMgr* asterisms = new AsterismMgr(hip_stars);
	asterisms->init();
	getModuleMgr().registerModule(asterisms);

	// Landscape, atmosphere & cardinal points section
	SplashScreen::showMessage(q_("Initializing landscape..."));
	LandscapeMgr* landscape = new LandscapeMgr();
	landscape->init();
	getModuleMgr().registerModule(landscape);
	// Photographic mode: the light domes of towns over the sky
	LightDomes* lightDomes = new LightDomes();
	lightDomes->init();
	getModuleMgr().registerModule(lightDomes);

	// Aurorae
	AuroraMgr* auroraMgr = new AuroraMgr();
	auroraMgr->init();
	getModuleMgr().registerModule(auroraMgr);
	// Only now we can switch to auto-landscape. (GH:#3550)
	if (landscape->getFlagLandscapeAutoSelection() && (confSettings->value("init_location/location", "auto").toString() == "auto"))
	{
		StelLocation loc=core->getCurrentLocation();
		QColor color=planetLocationMgr->getColorForCoordinates(loc.getLongitude(), loc.getLatitude());
		QString landscapeAutoName=QString("ZeroColor(%1)").arg(Vec3f(color).toStr());
		emit core->targetLocationChanged(loc, landscapeAutoName); // inform others about our next location. E.g., let LandscapeMgr load a new landscape.
	}

	SplashScreen::showMessage(q_("Initializing grid lines..."));
	GridLinesMgr* gridLines = new GridLinesMgr();
	gridLines->init();
	getModuleMgr().registerModule(gridLines);
	
	SplashScreen::showMessage(q_("Initializing special markers..."));
	SpecialMarkersMgr* specialMarkers = new SpecialMarkersMgr();
	specialMarkers->init();
	getModuleMgr().registerModule(specialMarkers);

	// Sporadic Meteors
	SplashScreen::showMessage(q_("Initializing sporadic meteors..."));
	SporadicMeteorMgr* meteors = new SporadicMeteorMgr(10, 72);
	meteors->init();
	getModuleMgr().registerModule(meteors);

	// User labels
	SplashScreen::showMessage(q_("Initializing user labels..."));
	LabelMgr* skyLabels = new LabelMgr();
	skyLabels->init();
	getModuleMgr().registerModule(skyLabels);

	SplashScreen::showMessage(q_("Initializing sky cultures..."));
	skyCultureMgr->init();

	// User markers
	SplashScreen::showMessage(q_("Initializing user markers..."));
	MarkerMgr* skyMarkers = new MarkerMgr();
	skyMarkers->init();
	getModuleMgr().registerModule(skyMarkers);

	// Init custom objects
	SplashScreen::showMessage(q_("Initializing custom objects..."));
	CustomObjectMgr* custObj = new CustomObjectMgr();
	custObj->init();
	getModuleMgr().registerModule(custObj);

	// Init highlights
	SplashScreen::showMessage(q_("Initializing highlights..."));
	HighlightMgr* hlMgr = new HighlightMgr();
	hlMgr->init();
	getModuleMgr().registerModule(hlMgr);

	// Init specific time
	SplashScreen::showMessage(q_("Initializing specific time..."));
	SpecificTimeMgr* specificTime = new SpecificTimeMgr();
	specificTime->init();
	getModuleMgr().registerModule(specificTime);

	//Create the script manager here, maybe some modules/plugins may want to connect to it
	//It has to be initialized later after all modules have been loaded by calling initScriptMgr
#ifdef ENABLE_SCRIPTING
	SplashScreen::showMessage(q_("Initializing scripting..."));
	scriptAPIProxy = new StelMainScriptAPIProxy(this);
	scriptMgr = new StelScriptMgr(this);
#endif

	SplashScreen::showMessage(q_("Initializing color scheme..."));
	// Initialisation of the color scheme
	emit colorSchemeChanged("default");
	setVisionModeNight(confSettings->value("viewing/flag_night", false).toBool());

	// Enable viewport effect at startup if he set
	setViewportEffect(confSettings->value("video/viewport_effect", "none").toString());

	SplashScreen::clearMessage();
	updateI18n();

	// Init actions.
	actionMgr->addAction("actionShow_Night_Mode", N_("Display Options"), N_("Night mode"), this, "nightMode", "Ctrl+N");

	// for backward compatibility
	const bool backwardDecimalDegrees = confSettings->value("gui/flag_show_decimal_degrees", false).toBool();
	setFlagUseDecDegreesCoords(confSettings->value("gui/flag_use_decimal_degrees_coords", backwardDecimalDegrees).toBool());
	setFlagUseDecDegreesOther(confSettings->value("gui/flag_use_decimal_degrees_other", backwardDecimalDegrees).toBool());
	setFlagSouthAzimuthUsage(confSettings->value("gui/flag_use_azimuth_from_south", false).toBool());
	setFlagUseNegativeHourAngles(confSettings->value("gui/flag_use_negative_hour_angles", false).toBool());
	setFlagPolarDistanceUsage(confSettings->value("gui/flag_use_polar_distance", false).toBool());
	setFlagUseFormattingOutput(confSettings->value("gui/flag_use_formatting_output", false).toBool());
	setFlagUseCCSDesignation(confSettings->value("gui/flag_use_ccs_designations", false).toBool());
	setFlagOverwriteInfoColor(confSettings->value("gui/flag_overwrite_info_color", false).toBool());	
	setOverwriteInfoColor(Vec3f(confSettings->value("color/info_text_color", "1.0,1.0,1.0").toString()));
	setDaylightInfoColor(Vec3f(confSettings->value("color/daylight_text_color", "0.0,0.0,0.0").toString()));

	// Animation
	animationScale = confSettings->value("gui/pointer_animation_speed", 1.).toDouble();

	if (StelMainView::getInstance().getGLInformation().isHighGraphicsMode)
	{
		ditherPatternTex = StelApp::getInstance().getTextureManager().getDitheringTexture(0);
	}
	setupPostProcessor();
	
#ifdef ENABLE_SPOUT
	//qDebug() << "Property spout is" << qApp->property("spout").toString();
	//qDebug() << "Property spoutName is" << qApp->property("spoutName").toString();
	if (qApp->property("spout").toString() != "none")
	{
		//if we are on windows and we have GLES, we are most likely on ANGLE
		bool isANGLE=QOpenGLContext::currentContext()->isOpenGLES();

		if (isANGLE)
		{
			qCritical() << "SPOUT: Does not run in ANGLE/OpenGL ES mode!";
		}
		else
		{
			SplashScreen::showMessage(q_("Initializing SPOUT sender..."));
			// Create the SpoutSender object.
			QString spoutName = qApp->property("spoutName").toString();
			if(spoutName.isEmpty())
				spoutName = "stellarium";

			spoutSender = new SpoutSender(spoutName);

			if (!spoutSender->isValid())
			{
				QMessageBox::warning(&StelMainView::getInstance(), "Stellarium SPOUT", q_("Cannot create Spout sender. See log for details."), QMessageBox::Ok);
				delete spoutSender;
				spoutSender = nullptr;
				qApp->setProperty("spout", "");
			}
			SplashScreen::clearMessage();
		}
	}
	else
	{
		qApp->setProperty("spout", "");
	}
#endif

	initialized = true;
}

// Load and initialize external modules (plugins)
void StelApp::initPlugIns()
{
	// Load dynamically all the modules found in the modules/ directories
	// which are configured to be loaded at startup
	const QList<StelModuleMgr::PluginDescriptor> pluginList=moduleMgr->getPluginsList();
	for (const auto& i : pluginList)
	{
		if (i.loadAtStartup==false)
			continue;
		SplashScreen::showMessage(QString("%1 \"%2\"...").arg(q_("Loading plugin"), q_(i.info.displayedName)));
		StelModule* m = moduleMgr->loadPlugin(i.info.id);
		if (m!=nullptr)
		{
			moduleMgr->registerModule(m, true);
			//load extensions after the module is registered
			moduleMgr->loadExtensions(i.info.id);
			m->init();
		}
	}
	SplashScreen::clearMessage();
}

void StelApp::deinit()
{
#ifdef 	ENABLE_SPOUT
	delete spoutSender;
	spoutSender = nullptr;
#endif
#ifdef ENABLE_SCRIPTING
	if (scriptMgr->scriptIsRunning())
		scriptMgr->stopScript();
#endif
	QCoreApplication::processEvents();
	getModuleMgr().unloadAllPlugins();
	QCoreApplication::processEvents();
	StelPainter::deinitGLShaders();
}


StelProgressController* StelApp::addProgressBar()
{
	StelProgressController* p = new StelProgressController(this);
	progressControllers.append(p);
	emit progressBarAdded(p);
	return p;
}

void StelApp::removeProgressBar(StelProgressController* p)
{
	progressControllers.removeOne(p);	
	emit progressBarRemoved(p);
	delete p;
}


void StelApp::update(double deltaTime)
{
	if (!initialized)
		return;

	++frame;
	frameTimeAccum+=deltaTime;
	if (frameTimeAccum > 1.)
	{
		// Calc the FPS rate every seconds
		fps=static_cast<float>(frame)/static_cast<float>(frameTimeAccum);
		frame = 0;
		frameTimeAccum=0.;
	}
		
	core->update(deltaTime);

	moduleMgr->update();

	// Send the event to every StelModule
	for (auto* i : moduleMgr->getCallOrders(StelModule::ActionUpdate))
	{
		i->update(deltaTime);
	}

	stelObjectMgr->update(deltaTime);
}

void StelApp::prepareRenderBuffer()
{
	if (!viewportEffect) return;
	if (!renderBuffer)
	{
		StelProjector::StelProjectorParams params = core->getCurrentStelProjectorParams();
		int w = params.viewportXywh[2];
		int h = params.viewportXywh[3];
		renderBuffer = new QOpenGLFramebufferObject(w, h, QOpenGLFramebufferObject::Depth); // we only need depth here
	}
	renderBuffer->bind();
}

void StelApp::requestPhotoLuminance(double x, double y)
{
	photoProbeRequested = true;
	photoProbeX = x;
	photoProbeY = y;
}

void StelApp::applyRenderBuffer(GLuint drawFbo)
{
	if (!renderBuffer) return;
	GL(gl->glBindFramebuffer(GL_FRAMEBUFFER, drawFbo));
	viewportEffect->paintViewportBuffer(renderBuffer);
}

namespace
{
//! GLSL code of the "camera" stage of the photographic mode.
//! The scene buffer holds linear light. A camera sensor records that light linearly,
//! scaled by the exposure, until it saturates. To avoid hard clipping edges we use a
//! soft shoulder: fully linear up to the knee, then a smooth roll-off towards 1.0.
//! The function is continuous and has slope 1 at the knee, so nothing changes below it.
QByteArray makeCameraResponseShader()
{
	return QByteArray("const highp float photoShoulderKnee = ") +
	       QByteArray::number(StelSkyDrawer::PHOTO_SHOULDER_KNEE, 'f', 6) + ";\n" + R"(
uniform bool photoMode;
uniform highp float photoExposure;
// Tone curve: 0 = linear with a soft shoulder; otherwise y = x^a / (x^(a*d)*b + c) up to the
// clipping point (white above). photoTone = (a, a*d, b, c), see StelSkyDrawer::PhotoToneParams
uniform int photoToneCurve;
uniform highp vec4 photoTone;
uniform highp float photoToneClip;
// White balance: light of the chosen color temperature -> display white (linear sRGB)
uniform highp mat3 photoWhiteBalance;
// Black and white film: the picture is the film's sensitivity to red, green and blue (photoMonoWeights)
uniform bool photoMonochrome;
uniform highp vec3 photoMonoWeights;
// Lens vignetting (see StelSkyDrawer::getPhotoLens()): window pixels
uniform bool photoVignetting;
uniform highp vec2 photoLensCenter;
uniform highp float photoLensCorner;
uniform highp float photoLensFocal;
uniform highp float photoLensPupil;  // radius of the pupil relative to the lens barrel (1 = wide open)
uniform highp float photoLensShift;  // offset of the pupil in the corner, in barrel radii

// Area of the intersection of two circles with radii r1, r2 and the distance d of their centers
highp float circleOverlap(highp float r1, highp float r2, highp float d)
{
	const highp float PI = 3.14159265358979;
	if (d >= r1 + r2) return 0.;
	if (d <= abs(r2 - r1)) return PI * min(r1, r2) * min(r1, r2);
	highp float a1 = r1*r1 * acos(clamp((d*d + r1*r1 - r2*r2) / (2.*d*r1), -1., 1.));
	highp float a2 = r2*r2 * acos(clamp((d*d + r2*r2 - r1*r1) / (2.*d*r2), -1., 1.));
	highp float k = (-d + r1 + r2) * (d + r1 - r2) * (d - r1 + r2) * (d + r1 + r2);
	return a1 + a2 - 0.5 * sqrt(max(k, 0.));
}
// Light that reaches the sensor at this pixel relative to the center of the image:
// natural vignetting of the field angle (rectilinear lens) and optical vignetting: off axis the lens
// barrel cuts the pupil (the "cat's eye"); stopping down shrinks the pupil and removes it.
// A thin lens gives cos^4; real (retrofocus) lenses show their pupil larger off axis, which makes it
// about cos^3 (e.g. about -1.5 EV in the corners of a 20 mm full frame lens stopped down).
highp float vignetting(highp vec2 fragCoord)
{
	highp float r = length(fragCoord - photoLensCenter);
	highp float t = r / photoLensFocal;
	highp float c2 = 1. / (1. + t*t);
	highp float pupilArea = 3.14159265358979 * photoLensPupil * photoLensPupil;
	highp float optical = circleOverlap(photoLensPupil, 1., photoLensShift * r / photoLensCorner) / pupilArea;
	return c2 * sqrt(c2) * optical;
}

// Sensor noise (see StelSkyDrawer::getPhotoNoiseParams()), in electrons of one photosite
uniform bool photoNoise;
uniform highp float photoNoiseWhite;   // electrons per scene unit (the sensor saturates at StelSkyDrawer::PHOTO_SENSOR_SATURATION)
uniform highp float photoNoiseRead2;   // read noise variance (e^2)
uniform highp float photoNoiseDark;    // mean dark signal over the exposure (e)
uniform highp float photoNoiseSites;   // photosites per screen pixel
uniform highp float photoNoiseHotProb; // probability of a hot photosite in a screen pixel
uniform highp float photoNoiseHot;     // mean dark signal of a hot photosite (e)
uniform highp float photoNoiseSeed;    // a new value every frame

// Four pseudo-random numbers in [0, 1) from a position (hash without sine, after D. Hoskins)
highp vec4 noiseHash(highp vec3 p)
{
	highp vec4 p4 = fract(vec4(p.xyzx) * vec4(0.1031, 0.1030, 0.0973, 0.1099));
	p4 += dot(p4, p4.wzxy + 33.33);
	return fract((p4.xxyz + p4.yzzw) * p4.zywx);
}
// The signal of the sensor (scene units) with its noise, per color channel:
// - photon (shot) noise: the photons arrive at random, Poisson statistics, variance = number of electrons
// - read noise of the electronics, and the noise of the dark current (thermal electrons)
// - hot pixels: a few photosites with a much larger dark current, always at the same place on the sensor
// A color sensor has a Bayer mosaic: of the photosites in a pixel 1/4 see red, 1/2 green, 1/4 blue, so red and
// blue are noisier. The mean dark signal is removed by the camera (black level from masked photosites).
highp vec3 sensorNoise(highp vec3 sensor, highp vec2 fragCoord)
{
	const highp float TWO_PI = 6.28318530718;
	highp vec3 sites = max(vec3(0.25, 0.5, 0.25) * photoNoiseSites, vec3(1.));
	highp vec3 electrons = max(sensor, vec3(0.)) * photoNoiseWhite;
	// standard deviation of the mean over the photosites of each channel
	highp vec3 sigma = sqrt((electrons + photoNoiseDark + photoNoiseRead2) / sites);
	highp vec4 h = noiseHash(vec3(floor(fragCoord), photoNoiseSeed));
	// Box-Muller: normally distributed numbers. The hash has steps of about 1e-4 (float precision),
	// so very small values are not reliable: limit them (the tail is cut at about 4.3 sigma)
	highp float r1 = sqrt(-2. * log(max(h.x, 1e-4)));
	highp float r2 = sqrt(-2. * log(max(h.z, 1e-4)));
	highp vec3 g = vec3(r1 * cos(TWO_PI * h.y), r1 * sin(TWO_PI * h.y), r2 * cos(TWO_PI * h.w));
	highp vec3 e = electrons + sigma * g;
	// Hot pixels (a fixed pattern of the sensor)
	highp vec4 hp = noiseHash(vec3(floor(fragCoord), 4099.37));
	// A small probability from two numbers (one alone is too coarse for that, see above)
	highp float q = sqrt(photoNoiseHotProb);
	if (hp.x < q && hp.w < q)
	{
		highp float hot = photoNoiseHot * -log(max(hp.y, 1e-4)); // exponential distribution of the dark currents
		highp vec3 channel = hp.z < 0.25 ? vec3(1., 0., 0.) : (hp.z < 0.75 ? vec3(0., 1., 0.) : vec3(0., 0., 1.));
		e += channel * hot / sites;
	}
	return e / photoNoiseWhite;
}

// Reciprocity failure of a film, per point after its brightness (see StelSkyDrawer::getPhotoReciprocityParams())
uniform bool photoRecip;
uniform highp float photoRecipTime;
uniform int photoRecipPoints;
uniform highp float photoRecipT[4];
uniform highp float photoRecipE[4];
uniform highp float photoRecipG[4];
uniform highp float photoRecipExponent;
const highp float photoMiddleGrey = )" + QByteArray::number(StelSkyDrawer::PHOTO_MIDDLE_GREY_EXPOSURE, 'f', 4) + R"(;
// The share of the exposure that counts at the time tn a middle grey would need at this intensity, and the green
// filter density of the data sheet at that time
highp vec2 recipFactor(highp float tn)
{
	if (tn <= photoRecipT[0])
		return vec2(1., 0.);
	for (int i = 1; i < 4; ++i)
	{
		if (i >= photoRecipPoints) break;
		if (tn <= photoRecipT[i])
		{
			highp float u = log(tn / photoRecipT[i-1]) / log(photoRecipT[i] / photoRecipT[i-1]);
			highp float te = photoRecipE[i-1] * pow(photoRecipE[i] / photoRecipE[i-1], u);
			return vec2(te / tn, mix(photoRecipG[i-1], photoRecipG[i], u));
		}
	}
	highp float tl = photoRecipT[0], el = photoRecipE[0], gl = photoRecipG[0];
	for (int i = 1; i < 4; ++i)
		if (i < photoRecipPoints) { tl = photoRecipT[i]; el = photoRecipE[i]; gl = photoRecipG[i]; }
	return vec2(el * pow(tn / tl, photoRecipExponent) / tn, gl);
}
highp vec3 filmReciprocity(highp vec3 x)
{
	highp vec3 tn = min(photoRecipTime * photoMiddleGrey / max(x, vec3(1e-20)), vec3(1e20));
	highp vec2 r = recipFactor(tn.r), g = recipFactor(tn.g), b = recipFactor(tn.b);
	return x * vec3(r.x, g.x * pow(10., -g.y), b.x);
}

// Film grain (see StelSkyDrawer::getPhotoGrainParams()): photoGrain 0 none, 1 slide, 2 negative and print
uniform int photoGrain;
uniform bool photoGrainMono;
uniform highp float photoGrainSigma;    // RMS density at density 1 in a pixel
uniform highp float photoGrainNegGamma; // negative: slope of its curve
uniform highp float photoGrainPrint;    // negative: slope of the paper
uniform highp float photoGrainToe;      // negative: exposure at the toe of its curve
const highp float photoSlideBase = )" + QByteArray::number(StelSkyDrawer::PHOTO_SLIDE_BASE_DENSITY, 'f', 3) + R"(;
const highp float photoSlideMax = )" + QByteArray::number(StelSkyDrawer::PHOTO_SLIDE_MAX_DENSITY, 'f', 3) + R"(;
const highp float photoNegativeFog = )" + QByteArray::number(StelSkyDrawer::PHOTO_NEGATIVE_FOG_DENSITY, 'f', 3) + R"(;
// The picture y (linear display values, after the tone curve) with the grain of the film; x is the exposure of
// the film (before the tone curve)
highp vec3 filmGrain(highp vec3 y, highp vec3 x, highp vec2 fragCoord)
{
	const highp float TWO_PI = 6.28318530718;
	highp vec4 h = noiseHash(vec3(floor(fragCoord), photoNoiseSeed + 2011.7));
	highp float r1 = sqrt(-2. * log(max(h.x, 1e-4)));
	highp float r2 = sqrt(-2. * log(max(h.z, 1e-4)));
	highp vec3 g = vec3(r1 * cos(TWO_PI * h.y), r1 * sin(TWO_PI * h.y), r2 * cos(TWO_PI * h.w));
	if (photoGrainMono)
		g = vec3(g.x);
	highp vec3 density;
	highp float gain = 1.;
	if (photoGrain == 1)
	{
		// A slide: the density of the picture itself
		density = photoSlideBase - log(max(y, vec3(pow(10., photoSlideBase - photoSlideMax)))) / log(10.);
	}
	else
	{
		// A negative: its density from the exposure; the paper multiplies the fluctuation
		density = photoNegativeFog + photoGrainNegGamma * log(max(x / photoGrainToe, vec3(1.))) / log(10.);
		gain = photoGrainPrint;
	}
	highp vec3 sigma = photoGrainSigma * sqrt(density) * gain;
	return min(y * pow(vec3(10.), sigma * g), vec3(1.));
}

// The frame of the sensor (see StelSkyDrawer::getPhotoFrame()): outside it the view is dimmed, like a viewfinder
// (the picture is what lies inside), with a thin line along its edge. On the encoded (display) values.
uniform bool photoFrameMask;
uniform highp vec4 photoFrame; // window coordinates: x0, y0, x1, y1
highp vec3 photoFrameMasked(highp vec3 encoded, highp vec2 fragCoord)
{
	if (!photoMode || !photoFrameMask)
		return encoded;
	highp vec2 d = max(photoFrame.xy - fragCoord, fragCoord - photoFrame.zw);
	highp float outside = max(d.x, d.y); // > 0 outside the frame (distance to its edge, pixels)
	highp vec3 c = outside > 0. ? 0.3 * encoded : encoded;
	highp float line = 1. - smoothstep(0.5, 1.5, abs(outside));
	return mix(c, vec3(0.7), 0.5 * line);
}

highp vec3 cameraResponse(highp vec3 lin)
{
	if(!photoMode) return lin;
	// min(): guard against an overflow to infinity (possible with half floats, as on OpenGL ES);
	// keep it finite so that mix() below cannot produce NaN (inf*0)
	highp float v = photoVignetting ? vignetting(gl_FragCoord.xy) : 1.;
	// The light on the sensor (the vignetting takes photons away); in manual mode the exposure compensation
	// (photoExposure) acts afterwards, like brightening the raw file: it does not add photons
	highp vec3 sensor = min(max(lin, vec3(0)), vec3(1e30)) * v;
	if (photoMonochrome)
		sensor = vec3(dot(sensor, photoMonoWeights));
	if (photoNoise)
		sensor = sensorNoise(sensor, gl_FragCoord.xy);
	highp vec3 x = min(max((photoWhiteBalance * sensor) * photoExposure, vec3(0)), vec3(1e30));
	if (photoRecip)
		x = filmReciprocity(x);
	if (photoToneCurve != 0)
	{
		// Each channel separately, as in a camera: bright colored light turns white when it saturates
		highp vec3 xc = min(x, vec3(photoToneClip));
		highp vec3 y = pow(xc, vec3(photoTone.x)) / (pow(xc, vec3(photoTone.y)) * photoTone.z + photoTone.w);
		y = mix(min(y, vec3(1)), vec3(1), step(vec3(photoToneClip), x));
		if (photoGrain != 0)
			y = filmGrain(y, x, gl_FragCoord.xy);
		return y;
	}
	const highp float knee = photoShoulderKnee;
	highp vec3 over = max(x - vec3(knee), vec3(0));
	highp vec3 shoulder = vec3(knee) + (1. - knee) * (vec3(1) - exp(-over / (1. - knee)));
	return mix(x, shoulder, step(vec3(knee), x));
}
)";
}
}

namespace
{
//! The tone curve of the photographic mode, for the camera stage (see makeCameraResponseShader())
void setPhotoToneUniforms(QOpenGLShaderProgram& program, const StelSkyDrawer& drawer, const StelProjector& prj)
{
	const StelSkyDrawer::PhotoLens lens = drawer.getPhotoLens(prj);
	program.setUniformValue("photoVignetting", lens.vignetting);
	program.setUniformValue("photoLensCenter", QVector2D(lens.center[0] + static_cast<float>(prj.getViewportPosX()),
	                                                     lens.center[1] + static_cast<float>(prj.getViewportPosY())));
	program.setUniformValue("photoLensCorner", lens.corner);
	program.setUniformValue("photoLensFocal", lens.focal);
	program.setUniformValue("photoLensPupil", lens.pupil);
	program.setUniformValue("photoLensShift", StelSkyDrawer::PHOTO_VIGNETTING_SHIFT);
	const Vec4f frame = drawer.getPhotoFrame(prj);
	program.setUniformValue("photoFrameMask", drawer.getFlagPhotoFrameMask());
	program.setUniformValue("photoFrame", QVector4D(frame[0], frame[1], frame[0] + frame[2], frame[1] + frame[3]));
	const StelSkyDrawer::PhotoToneParams& p = drawer.getPhotoToneParams();
	program.setUniformValue("photoToneCurve", drawer.getPhotoActiveToneCurve());
	const Vec3f mono = drawer.getPhotoFilmMonoWeights();
	program.setUniformValue("photoMonochrome", mono[0] + mono[1] + mono[2] > 0.f);
	program.setUniformValue("photoMonoWeights", mono[0], mono[1], mono[2]);
	program.setUniformValue("photoTone", p.a, p.ad, p.b, p.c);
	program.setUniformValue("photoToneClip", p.clip);
	program.setUniformValue("photoWhiteBalance", QMatrix3x3(drawer.getPhotoWhiteBalanceMatrix()));
	const StelSkyDrawer::PhotoNoiseParams noise = drawer.getPhotoNoiseParams(prj);
	program.setUniformValue("photoNoise", noise.enabled);
	program.setUniformValue("photoNoiseWhite", noise.white);
	program.setUniformValue("photoNoiseRead2", noise.read2);
	program.setUniformValue("photoNoiseDark", noise.dark);
	program.setUniformValue("photoNoiseSites", noise.sites);
	program.setUniformValue("photoNoiseHotProb", noise.hotProbability);
	program.setUniformValue("photoNoiseHot", noise.hot);
	const StelSkyDrawer::PhotoReciprocityParams recip = drawer.getPhotoReciprocityParams();
	program.setUniformValue("photoRecip", recip.enabled);
	program.setUniformValue("photoRecipTime", recip.time);
	program.setUniformValue("photoRecipPoints", recip.points);
	program.setUniformValueArray("photoRecipT", recip.times, 4, 1);
	program.setUniformValueArray("photoRecipE", recip.effective, 4, 1);
	program.setUniformValueArray("photoRecipG", recip.green, 4, 1);
	program.setUniformValue("photoRecipExponent", recip.exponent);
	const StelSkyDrawer::PhotoGrainParams grain = drawer.getPhotoGrainParams(prj);
	program.setUniformValue("photoNoiseSeed", noise.enabled ? noise.seed : grain.seed);
	program.setUniformValue("photoGrain", grain.mode);
	program.setUniformValue("photoGrainMono", grain.mono);
	program.setUniformValue("photoGrainSigma", grain.sigma);
	program.setUniformValue("photoGrainNegGamma", grain.negativeGamma);
	program.setUniformValue("photoGrainPrint", grain.printGamma);
	program.setUniformValue("photoGrainToe", grain.toe);
}
}

void StelApp::setupPostProcessor()
{
	postProcessorVBO.reset(new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer));
	postProcessorVBO->create();
	postProcessorVBO->bind();
	const GLfloat vertices[]=
	{
		// full screen quad
		-1, -1,
		 1, -1,
		-1,  1,
		 1,  1,
	};
	GL(gl->glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW));

	postProcessorVAO.reset(new QOpenGLVertexArrayObject);
	postProcessorVAO->create();
	postProcessorVAO->bind();
	gl->glVertexAttribPointer(0, 2, GL_FLOAT, false, 0, 0);
	postProcessorVBO->release();
	gl->glEnableVertexAttribArray(0);
	postProcessorVAO->release();

	postProcessorProgram.reset(new QOpenGLShaderProgram);
	postProcessorProgram->addShaderFromSourceCode(QOpenGLShader::Vertex,
		StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + R"(
ATTRIBUTE vec3 vertex;
VARYING vec2 texcoord;
void main()
{
	gl_Position = vec4(vertex, 1.);
	texcoord = 0.5*vertex.xy+0.5;
}
)");
	postProcessorProgram->addShaderFromSourceCode(QOpenGLShader::Fragment,
		StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) +
		makeSRGBUtilsShader() +
		makeDitheringShader() +
		makeCameraResponseShader() + R"(
VARYING vec2 texcoord;
uniform sampler2D tex;
// Photographic mode: the lens halo, computed from the whole image (see StelPhotoBloom)
uniform bool bloomEnabled;
uniform sampler2D bloomTex;
uniform highp float bloomAmount;   // halo fraction, relative to the light left in the image
uniform highp float bloomIdentity; // part of the halo profile narrower than a pixel
// Photographic mode: the aureole of the haze (see StelPhotoBloom), times its share over the screen
uniform bool aureoleEnabled;
uniform sampler2D aureoleTex;
uniform highp float aureoleScale;  // the light left in the image -> all the light
uniform bool spikeEnabled;
uniform sampler2D spikeTex;
uniform highp float spikeAmount;   // spike fraction, relative to the light left in the image
// Film: the halation (see StelPhotoBloom), at a coarser level of the pyramid
uniform bool halationEnabled;
uniform sampler2D halationTex;
uniform highp vec2 halationScale;  // scene texture coordinates -> those of the halation image
uniform highp vec3 halationAmount; // share of the light per color, relative to the light left in the image

void main()
{
	vec4 rgba = texture2D(tex, texcoord);
	highp vec3 extra = vec3(0);
	if (bloomEnabled)
		extra += bloomAmount * (texture2D(bloomTex, texcoord).rgb + bloomIdentity * rgba.rgb);
	if (spikeEnabled)
		extra += spikeAmount * texture2D(spikeTex, texcoord).rgb;
	if (aureoleEnabled)
		extra += aureoleScale * texture2D(aureoleTex, texcoord).rgb;
	if (halationEnabled)
		extra += halationAmount * texture2D(halationTex, texcoord * halationScale).rgb;
	rgba.rgb += extra;
	FRAG_COLOR = vec4(dither(photoFrameMasked(linearToSRGB(cameraResponse(rgba.rgb)), gl_FragCoord.xy)), rgba.a);
}
)");
	StelPainter::linkProg(postProcessorProgram.get(), "Post-processing program");
	postProcessorProgram->bind();
	postProcessorUniformLocations.tex           = postProcessorProgram->uniformLocation("tex");
	postProcessorUniformLocations.rgbMaxValue   = postProcessorProgram->uniformLocation("rgbMaxValue");
	postProcessorUniformLocations.ditherPattern = postProcessorProgram->uniformLocation("ditherPattern");
	postProcessorUniformLocations.photoMode     = postProcessorProgram->uniformLocation("photoMode");
	postProcessorUniformLocations.photoExposure = postProcessorProgram->uniformLocation("photoExposure");
	postProcessorUniformLocations.bloomEnabled  = postProcessorProgram->uniformLocation("bloomEnabled");
	postProcessorUniformLocations.bloomTex      = postProcessorProgram->uniformLocation("bloomTex");
	postProcessorUniformLocations.bloomAmount   = postProcessorProgram->uniformLocation("bloomAmount");
	postProcessorUniformLocations.bloomIdentity = postProcessorProgram->uniformLocation("bloomIdentity");
	postProcessorUniformLocations.aureoleEnabled = postProcessorProgram->uniformLocation("aureoleEnabled");
	postProcessorUniformLocations.aureoleTex = postProcessorProgram->uniformLocation("aureoleTex");
	postProcessorUniformLocations.aureoleScale = postProcessorProgram->uniformLocation("aureoleScale");
	postProcessorUniformLocations.spikeEnabled  = postProcessorProgram->uniformLocation("spikeEnabled");
	postProcessorUniformLocations.spikeTex      = postProcessorProgram->uniformLocation("spikeTex");
	postProcessorUniformLocations.spikeAmount   = postProcessorProgram->uniformLocation("spikeAmount");
	postProcessorProgram->release();

#if !QT_CONFIG(opengles2)
	if(StelMainView::getInstance().getGLInformation().isHighGraphicsMode)
	{
		postProcessorProgramMS.reset(new QOpenGLShaderProgram);
		postProcessorProgramMS->addShaderFromSourceCode(QOpenGLShader::Vertex,
			StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) + R"(
ATTRIBUTE vec3 vertex;
void main()
{
	gl_Position = vec4(vertex, 1.);
}
)");
		postProcessorProgramMS->addShaderFromSourceCode(QOpenGLShader::Fragment,
			StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) +
			makeSRGBUtilsShader() +
			makeDitheringShader() +
			makeCameraResponseShader() + R"(
uniform sampler2DMS tex;
uniform int numMultiSamples;

void main()
{
	vec4 rgba = vec4(0);
	for(int n = 0; n < numMultiSamples; ++n)
		rgba += texelFetch(tex, ivec2(gl_FragCoord.xy), n);
	rgba /= float(numMultiSamples);
	FRAG_COLOR = vec4(dither(photoFrameMasked(linearToSRGB(cameraResponse(rgba.rgb)), gl_FragCoord.xy)), rgba.a);
}
)");
		StelPainter::linkProg(postProcessorProgramMS.get(), "Multisampled post-processing program");
		postProcessorProgramMS->bind();
		postProcessorUniformLocationsMS.tex               = postProcessorProgramMS->uniformLocation("tex");
		postProcessorUniformLocationsMS.rgbMaxValue       = postProcessorProgramMS->uniformLocation("rgbMaxValue");
		postProcessorUniformLocationsMS.ditherPattern     = postProcessorProgramMS->uniformLocation("ditherPattern");
		postProcessorUniformLocationsMS.numMultiSamples   = postProcessorProgramMS->uniformLocation("numMultiSamples");
		postProcessorUniformLocationsMS.photoMode         = postProcessorProgramMS->uniformLocation("photoMode");
		postProcessorUniformLocationsMS.photoExposure     = postProcessorProgramMS->uniformLocation("photoExposure");
		postProcessorProgramMS->release();
	}
#endif
}

void StelApp::highGraphicsModeDraw()
{
	const auto targetFBO = currentFbo;
	StelProjector::StelProjectorParams params = core->getCurrentStelProjectorParams();
	const auto w = params.viewportXywh[2] * params.devicePixelsPerPixel;
	const auto h = params.viewportXywh[3] * params.devicePixelsPerPixel;
	StelOpenGL::checkGLErrors(__FILE__, __LINE__);

	// The photographic mode needs room above 1.0 in the scene buffer, so that the
	// camera pass can apply the exposure before saturation. Otherwise we keep the
	// integer format that has always been used on desktop OpenGL.
	const StelSkyDrawer* skyDrawer = core->getSkyDrawer();
	const bool photoMode = skyDrawer->getFlagPhotographicMode();
#if QT_CONFIG(opengles2)
	const bool wantFloatScene = true;
#else
	const bool wantFloatScene = photoMode;
#endif
	const float photoExposure = photoMode ? skyDrawer->getPhotoExposureFactor() : 1.f;
	// How this frame is exposed (the automatic exposure may change it after the frame has been measured)
	const double photoSceneScale = skyDrawer->getPhotoSceneScale();
	const double photoExposureTime = skyDrawer->getPhotoEffectiveExposureTime();

	if(!sceneFBO || sceneFBO->size() != QSize(w,h) || sceneFBOIsFloat != wantFloatScene)
	{
		GLint viewport[4] = {};
		GL(gl->glGetIntegerv(GL_VIEWPORT, viewport));
		qInfo() << "OpenGL viewport size:" << viewport[2] << "x" << viewport[3];

		qInfo().nospace() << "Creating scene FBO with size " << w << "x" << h;

		// The photographic mode needs a large range: the Sun's disk can reach 1e10 times the
		// saturation level, and its halo (bloom) must be computed from the full value.
#if QT_CONFIG(opengles2)
		const auto internalFormat = GL_RGBA16F;
#else
		const auto internalFormat = wantFloatScene ? GL_RGBA32F : GL_RGBA16;
#endif
		sceneFBOIsFloat = wantFloatScene;
		QOpenGLFramebufferObjectFormat format;
		format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
		format.setInternalTextureFormat(internalFormat);
		sceneFBO.reset(new QOpenGLFramebufferObject(w, h, format));
#if !QT_CONFIG(opengles2)
		GLint maxSamples = 1;
		GL(gl->glGetIntegerv(GL_MAX_SAMPLES, &maxSamples));
		const auto samples = confSettings->value("video/multisampling", 0).toInt();
		numMultiSamples = std::min(samples, maxSamples);
		if(numMultiSamples > 1)
		{
			const auto gl = StelOpenGL::highGraphicsFunctions();
			if(sceneMultisampledFBO)
			{
				GL(gl->glDeleteFramebuffers(1, &sceneMultisampledFBO));
				GL(gl->glDeleteTextures(1, &sceneMultisampledTex));
				GL(gl->glDeleteRenderbuffers(1, &sceneMultisampledRenderbuffer));
			}
			GL(gl->glGenFramebuffers(1, &sceneMultisampledFBO));
			GL(gl->glGenTextures(1, &sceneMultisampledTex));
			GL(gl->glGenRenderbuffers(1, &sceneMultisampledRenderbuffer));

			GL(gl->glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, sceneMultisampledTex));
			GL(gl->glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, numMultiSamples,
			                                        internalFormat, w, h, true));

			GL(gl->glBindRenderbuffer(GL_RENDERBUFFER, sceneMultisampledRenderbuffer));
			GL(gl->glRenderbufferStorageMultisample(GL_RENDERBUFFER, numMultiSamples,
			                                                 GL_DEPTH24_STENCIL8, w, h));

			GL(gl->glBindFramebuffer(GL_FRAMEBUFFER, sceneMultisampledFBO));
			GL(gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			                              GL_TEXTURE_2D_MULTISAMPLE, sceneMultisampledTex, 0));
			GL(gl->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
			                                 GL_RENDERBUFFER, sceneMultisampledRenderbuffer));
			const auto status = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER);
			if(status != GL_FRAMEBUFFER_COMPLETE)
			{
				qCritical().nospace() << __FILE__ << ":" << __LINE__
				                      << ": warning: framebuffer incomplete, status: "
				                      << status;
			}
		}
#endif
	}

	if(sceneMultisampledFBO)
	{
		GL(gl->glBindFramebuffer(GL_FRAMEBUFFER, sceneMultisampledFBO));
		currentFbo = sceneMultisampledFBO;
	}
	else
	{
		sceneFBO->bind();
		currentFbo = sceneFBO->handle();
	}
	StelOpenGL::checkGLErrors(__FILE__, __LINE__);

	GL(gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));

	const QList<StelModule*> modules = moduleMgr->getCallOrders(StelModule::ActionDraw);

	for(auto* module : modules)
	{
		module->draw(core);
	}

	// Photographic mode: halo and spikes of the bright bodies, over everything (also the landscape)
	if (photoMode)
		core->getSkyDrawer()->drawDeferredPhotoBodies(core);

	// Photographic mode: the lens halo of the whole image (bloom)
	bool bloomDone = false;
	const bool bloomActive = photoMode && skyDrawer->getFlagPhotoBloomActive();
	const bool aureoleActive = photoMode && skyDrawer->getFlagPhotoAureoleActive();
	if (bloomActive || aureoleActive)
	{
#if !QT_CONFIG(opengles2)
		if (sceneMultisampledFBO)
		{
			// The halo needs a plain texture: resolve the multisampled scene into sceneFBO
			const auto glh = StelOpenGL::highGraphicsFunctions();
			GL(glh->glBindFramebuffer(GL_READ_FRAMEBUFFER, sceneMultisampledFBO));
			GL(glh->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sceneFBO->handle()));
			GL(glh->glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST));
			GL(glh->glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO->handle()));
		}
#endif
		if (!photoBloom)
			photoBloom.reset(new StelPhotoBloom);
		const bool spikes = bloomActive && skyDrawer->getPhotoSpikeFractionEffective() > 0.f;
		const StelSkyDrawer::PhotoHalationParams halation = skyDrawer->getPhotoHalationParams(*core->getProjection(StelCore::FrameJ2000));
		float aureoleCore = 0.f, aureoleExtent = 0.f;
		if (aureoleActive)
		{
			skyDrawer->getPhotoAureoleSize(*core->getProjection(StelCore::FrameAltAz), aureoleCore, aureoleExtent);
			// the share of the light of each point that goes into its aureole (from its airmass)
			constexpr int GW = 64, GH = 36;
			const std::vector<float> share = skyDrawer->getPhotoAureoleMap(GW, GH, static_cast<int>(w), static_cast<int>(h));
			if (!photoAureoleMapTex)
				GL(gl->glGenTextures(1, &photoAureoleMapTex));
			GL(gl->glBindTexture(GL_TEXTURE_2D, photoAureoleMapTex));
			GL(gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, GW, GH, 0, GL_RED, GL_FLOAT, share.data()));
			GL(gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
			GL(gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
			GL(gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
			GL(gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
			GL(gl->glBindTexture(GL_TEXTURE_2D, 0));
		}
		bloomDone = photoBloom->process(sceneFBO->texture(), static_cast<int>(w), static_cast<int>(h),
		                                bloomActive ? skyDrawer->getPhotoHaloAlpha() : 0.f, StelSkyDrawer::getPhotoHaloBeta(),
		                                spikes ? skyDrawer->getPhotoSpikeLines() : 0,
		                                StelSkyDrawer::getPhotoSpikeAngle(), skyDrawer->getPhotoSpikeLength(),
		                                postProcessorVAO.get(), bloomActive ? halation.radius : 0.f, halation.density,
		                                aureoleCore, aureoleExtent, aureoleActive ? photoAureoleMapTex : 0);
	}

	// The meter and the luminance probe need a plain texture: the multisampled scene resolved into sceneFBO (once)
	bool sceneResolved = bloomDone || !sceneMultisampledFBO;
	const auto resolveScene = [&]()
	{
		if (sceneResolved)
			return;
		sceneResolved = true;
#if !QT_CONFIG(opengles2)
		const auto glh = StelOpenGL::highGraphicsFunctions();
		GL(glh->glBindFramebuffer(GL_READ_FRAMEBUFFER, sceneMultisampledFBO));
		GL(glh->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sceneFBO->handle()));
		GL(glh->glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST));
		GL(glh->glBindFramebuffer(GL_FRAMEBUFFER, sceneFBO->handle()));
#endif
	};

	// Photographic mode: the exposure meter of the automatic exposure and white balance
	if (photoMode && (skyDrawer->getFlagPhotoAutoExposureActive() || skyDrawer->getFlagPhotoAutoWhiteBalanceActive()))
	{
		resolveScene();
		if (!photoMeter)
			photoMeter.reset(new StelPhotoMeter);
		// First the reading of the previous frame (its transfer is finished by now), then this frame
		StelPhotoMeter::Reading reading;
		if (photoMeter->takeReading(reading, skyDrawer->getPhotoMeterClipLevel()))
		{
			const qint64 now = QDateTime::currentMSecsSinceEpoch();
			const double dt = photoMeterLastMs < 0 ? 0. : 0.001*static_cast<double>(now - photoMeterLastMs);
			photoMeterLastMs = now;
			core->getSkyDrawer()->applyPhotoMeterReading(reading, dt);
		}
		// Only the frame of the sensor: what lies outside is not in the picture
		const Vec4f frame = skyDrawer->getPhotoFrame(*core->getProjection(StelCore::FrameJ2000));
		const int fx0 = qBound(0, static_cast<int>(std::floor(frame[0])), static_cast<int>(w) - 1);
		const int fy0 = qBound(0, static_cast<int>(std::floor(frame[1])), static_cast<int>(h) - 1);
		const int fw = qBound(1, static_cast<int>(std::ceil(frame[2])), static_cast<int>(w) - fx0);
		const int fh = qBound(1, static_cast<int>(std::ceil(frame[3])), static_cast<int>(h) - fy0);
		photoMeter->measure(sceneFBO->texture(), fw, fh, photoSceneScale, photoExposureTime, postProcessorVAO.get(),
		                    fx0, fy0, static_cast<int>(w), static_cast<int>(h));
	}
	else
	{
		if (photoMeter)
			photoMeter->reset();
		photoMeterLastMs = -1;
		core->getSkyDrawer()->applyPhotoMeterReading(StelPhotoMeter::Reading(), 0.); // marks the next reading as the first
	}

	// Photographic mode: the sky quality meter of the information line (the zenith), a few times per second
	if (photoMode && skyDrawer->getFlagPhotoInfoLine())
	{
		if (!photoSqmMeter)
			photoSqmMeter.reset(new StelPhotoMeter(StelPhotoMeter::AreaMeans));
		StelPhotoMeter::Reading reading;
		if (photoSqmMeter->takeReading(reading))
			core->getSkyDrawer()->applyPhotoSqmReading(reading);
		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		if (photoSqmLastMs < 0 || now - photoSqmLastMs >= PHOTO_SQM_INTERVAL_MS)
		{
			resolveScene();
			photoSqmMeter->measure(sceneFBO->texture(), static_cast<int>(w), static_cast<int>(h), photoSceneScale, photoExposureTime,
			                       postProcessorVAO.get());
			photoSqmLastMs = now;
		}
	}
	else if (photoSqmMeter)
	{
		photoSqmMeter->reset();
		photoSqmLastMs = -1;
		core->getSkyDrawer()->applyPhotoSqmReading(StelPhotoMeter::Reading());
	}

	// Photographic mode: the luminance at a point (requestPhotoLuminance(), e.g. under the mouse pointer), a few
	// times per second. Each result is taken in a later frame, when its transfer is done.
	if (photoMode && photoProbeRequested)
	{
		if (!photoProbe)
			photoProbe.reset(new StelPhotoProbe);
		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		double luminance = 0.;
		if (photoProbe->takeReading(luminance))
		{
			photoProbeLuminance = luminance;
			photoProbeResultMs = now;
		}
		if (!photoProbe->isPending() && (photoProbeLastMs < 0 || now - photoProbeLastMs >= PHOTO_PROBE_INTERVAL_MS))
		{
			resolveScene();
			photoProbe->measure(sceneFBO->handle(), static_cast<int>(std::floor(photoProbeX)), static_cast<int>(std::floor(photoProbeY)),
			                    static_cast<int>(w), static_cast<int>(h), photoSceneScale);
			photoProbeLastMs = now;
		}
	}
	else
	{
		if (photoProbe)
			photoProbe->reset();
		photoProbeLastMs = -1;
		photoProbeLuminance = -1.;
	}
	photoProbeRequested = false;

	if(sceneMultisampledFBO && !bloomDone)
	{
		GL(gl->glBindFramebuffer(GL_FRAMEBUFFER, targetFBO));
		postProcessorProgramMS->bind();

		const int sceneTexSampler = 0;
		GL(gl->glActiveTexture(GL_TEXTURE0 + sceneTexSampler));
		GL(gl->glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, sceneMultisampledTex));
		postProcessorProgramMS->setUniformValue(postProcessorUniformLocationsMS.tex, sceneTexSampler);

		const int ditherTexSampler = 1;
		ditherPatternTex->bind(ditherTexSampler);
		postProcessorProgramMS->setUniformValue(postProcessorUniformLocationsMS.ditherPattern, ditherTexSampler);
		const auto rgbMaxValue=calcRGBMaxValue(core->getDitheringMode());
		postProcessorProgramMS->setUniformValue(postProcessorUniformLocationsMS.rgbMaxValue,
		                                        rgbMaxValue[0], rgbMaxValue[1], rgbMaxValue[2]);
		postProcessorProgramMS->setUniformValue(postProcessorUniformLocationsMS.numMultiSamples, numMultiSamples);
		postProcessorProgramMS->setUniformValue(postProcessorUniformLocationsMS.photoMode, photoMode);
		postProcessorProgramMS->setUniformValue(postProcessorUniformLocationsMS.photoExposure, photoExposure);
		setPhotoToneUniforms(*postProcessorProgramMS, *skyDrawer, *core->getProjection(StelCore::FrameJ2000));
	}
	else
	{
		GL(gl->glBindFramebuffer(GL_FRAMEBUFFER, targetFBO));
		postProcessorProgram->bind();

		const int sceneTexSampler = 0;
		GL(gl->glActiveTexture(GL_TEXTURE0 + sceneTexSampler));
		GL(gl->glBindTexture(GL_TEXTURE_2D, sceneFBO->texture()));
		postProcessorProgram->setUniformValue(postProcessorUniformLocations.tex, sceneTexSampler);

		const int ditherTexSampler = 1;
		ditherPatternTex->bind(ditherTexSampler);
		postProcessorProgram->setUniformValue(postProcessorUniformLocations.ditherPattern, ditherTexSampler);
		const auto rgbMaxValue=calcRGBMaxValue(core->getDitheringMode());
		postProcessorProgram->setUniformValue(postProcessorUniformLocations.rgbMaxValue,
		                                      rgbMaxValue[0], rgbMaxValue[1], rgbMaxValue[2]);
		postProcessorProgram->setUniformValue(postProcessorUniformLocations.photoMode, photoMode);
		postProcessorProgram->setUniformValue(postProcessorUniformLocations.photoExposure, photoExposure);
		setPhotoToneUniforms(*postProcessorProgram, *skyDrawer, *core->getProjection(StelCore::FrameJ2000));

		const bool haloOn = bloomDone && bloomActive && skyDrawer->getPhotoHaloStrength() > 0.;
		const bool aureoleOn = bloomDone && photoBloom->aureoleTexture() != 0;
		postProcessorProgram->setUniformValue(postProcessorUniformLocations.aureoleEnabled, aureoleOn);
		postProcessorProgram->setUniformValue(postProcessorUniformLocations.aureoleTex, 5);
		if (aureoleOn)
		{
			GL(gl->glActiveTexture(GL_TEXTURE0 + 5));
			GL(gl->glBindTexture(GL_TEXTURE_2D, photoBloom->aureoleTexture()));
			GL(gl->glActiveTexture(GL_TEXTURE0));
			// the stars are drawn with the share of their light that is not in the halo and spikes
			const float coreFraction = bloomActive ? skyDrawer->getPhotoCoreFraction() : 1.f;
			postProcessorProgram->setUniformValue(postProcessorUniformLocations.aureoleScale, coreFraction > 0.f ? 1.f/coreFraction : 0.f);
		}
		const bool spikesOn = bloomDone && photoBloom->spikeTexture() != 0;
		postProcessorProgram->setUniformValue(postProcessorUniformLocations.bloomEnabled, haloOn);
		postProcessorProgram->setUniformValue(postProcessorUniformLocations.spikeEnabled, spikesOn);
		const int bloomTexSampler = 2;
		const int spikeTexSampler = 3;
		if (spikesOn)
		{
			GL(gl->glActiveTexture(GL_TEXTURE0 + spikeTexSampler));
			GL(gl->glBindTexture(GL_TEXTURE_2D, photoBloom->spikeTexture()));
			GL(gl->glActiveTexture(GL_TEXTURE0));
			const float coreFraction = skyDrawer->getPhotoCoreFraction();
			postProcessorProgram->setUniformValue(postProcessorUniformLocations.spikeAmount,
			                                      coreFraction > 0.f ? skyDrawer->getPhotoSpikeFractionEffective()/coreFraction : 0.f);
		}
		postProcessorProgram->setUniformValue(postProcessorUniformLocations.spikeTex, spikeTexSampler);
		if (haloOn)
		{
			GL(gl->glActiveTexture(GL_TEXTURE0 + bloomTexSampler));
			GL(gl->glBindTexture(GL_TEXTURE_2D, photoBloom->haloTexture()));
			GL(gl->glActiveTexture(GL_TEXTURE0));
			const float coreFraction = skyDrawer->getPhotoCoreFraction();
			postProcessorProgram->setUniformValue(postProcessorUniformLocations.bloomAmount,
			                                      coreFraction > 0.f ? static_cast<float>(skyDrawer->getPhotoHaloStrength())/coreFraction : 0.f);
			postProcessorProgram->setUniformValue(postProcessorUniformLocations.bloomIdentity, photoBloom->identityWeight());
		}
		postProcessorProgram->setUniformValue(postProcessorUniformLocations.bloomTex, bloomTexSampler);
		// Film: the halation
		const GLuint halationTex = bloomDone ? photoBloom->halationTexture() : 0;
		const int halationTexSampler = 4;
		postProcessorProgram->setUniformValue("halationEnabled", halationTex != 0);
		postProcessorProgram->setUniformValue("halationTex", halationTexSampler);
		if (halationTex)
		{
			GL(gl->glActiveTexture(GL_TEXTURE0 + halationTexSampler));
			GL(gl->glBindTexture(GL_TEXTURE_2D, halationTex));
			GL(gl->glActiveTexture(GL_TEXTURE0));
			const StelSkyDrawer::PhotoHalationParams halation = skyDrawer->getPhotoHalationParams(*core->getProjection(StelCore::FrameJ2000));
			const float coreFraction = skyDrawer->getPhotoCoreFraction();
			const Vec3f a = coreFraction > 0.f ? halation.gain/coreFraction : Vec3f(0.f);
			postProcessorProgram->setUniformValue("halationAmount", a[0], a[1], a[2]);
			postProcessorProgram->setUniformValue("halationScale", photoBloom->halationScale());
		}
	}

	postProcessorVAO->bind();
	GL(gl->glEnable(GL_BLEND));
	GL(gl->glBlendFunc(GL_ONE,GL_ONE));
	GL(gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
	GL(gl->glDisable(GL_BLEND));
	postProcessorVAO->release();

	// Photographic mode: the camera settings in use, on top of the finished picture
	if (photoMode)
		skyDrawer->drawPhotoInfoLine(core);
}

//! Main drawing function called at each frame
void StelApp::draw()
{
	if (!initialized)
		return;

	//find out which framebuffer is the current one
	//this is usually NOT the "zero" FBO, but one provided by QOpenGLWidget
	GLint drawFbo;
	GL(gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &drawFbo));

	prepareRenderBuffer();
	currentFbo = renderBuffer ? renderBuffer->handle() : static_cast<GLuint>(drawFbo);

	core->preDraw();

	if(StelMainView::getInstance().getGLInformation().isHighGraphicsMode)
	{
		highGraphicsModeDraw();
	}
	else
	{
		const QList<StelModule*> modules = moduleMgr->getCallOrders(StelModule::ActionDraw);
		for (auto* module : modules)
		{
			module->draw(core);
		}
	}

	core->postDraw();
#ifdef ENABLE_SPOUT
	// At this point, the sky scene has been drawn, but no GUI panels.
	if(spoutSender)
		spoutSender->captureAndSendFrame(static_cast<GLuint>(drawFbo));
#endif
	applyRenderBuffer(static_cast<GLuint>(drawFbo));
}

/*************************************************************************
 Call this when the virtual size of the GL window has changed
*************************************************************************/
void StelApp::glWindowHasBeenResized(const QRectF& rect)
{
	// Remove the effect before resizing the core, or things get messy.
	QString effect = getViewportEffect();
	setViewportEffect("none");
	if (core)
	{
		core->windowHasBeenResized(rect.x(), rect.y(), rect.width(), rect.height());
	}
	else
	{
		saveProjW = rect.width();
		saveProjH = rect.height();
	}
	// Force to recreate the viewport effect if any.
	setViewportEffect(effect);
#ifdef ENABLE_SPOUT
	if (spoutSender)
		spoutSender->resize(static_cast<uint>(rect.width()),static_cast<uint>(rect.height()));
#endif
}

/*************************************************************************
 Call this when the physical size of the GL window has changed
*************************************************************************/
void StelApp::glPhysicalWindowHasBeenResized(const QRectF& rect)
{
#ifdef ENABLE_SPOUT
	if (spoutSender)
		spoutSender->resize(static_cast<uint>(rect.width()),static_cast<uint>(rect.height()));
#else
	Q_UNUSED(rect)
#endif
}

// Handle mouse clics
void StelApp::handleClick(QMouseEvent* inputEvent)
{
	QPointF pos = inputEvent->pos();
	qreal x, y;
	x = pos.x();
	y = pos.y();
	if (viewportEffect)
		viewportEffect->distortXY(x, y);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	QMouseEvent event(inputEvent->type(), QPointF(x,y) * devicePixelsPerPixel, inputEvent->globalPosition() * devicePixelsPerPixel, inputEvent->button(), inputEvent->buttons(), inputEvent->modifiers());
#else
	QMouseEvent event(inputEvent->type(), QPoint(qRound(x*devicePixelsPerPixel), qRound(y*devicePixelsPerPixel)), inputEvent->button(), inputEvent->buttons(), inputEvent->modifiers());
#endif
	event.setAccepted(false);
	
	// Send the event to every StelModule
	for (auto* i : moduleMgr->getCallOrders(StelModule::ActionHandleMouseClicks))
	{
		i->handleMouseClicks(&event);
		if (event.isAccepted())
		{
			inputEvent->setAccepted(true);
			return;
		}
	}
}

// Handle mouse wheel.
void StelApp::handleWheel(QWheelEvent* event)
{
	event->setAccepted(false);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
	QWheelEvent deltaEvent(event->position()*devicePixelsPerPixel,
			       event->globalPosition()*devicePixelsPerPixel,
			       event->pixelDelta(), event->angleDelta(), event->buttons(), event->modifiers(), Qt::ScrollUpdate, false);
#else
	QWheelEvent deltaEvent(QPoint(qRound(event->pos().x()*devicePixelsPerPixel), qRound(event->pos().y()*devicePixelsPerPixel)),
			       QPoint(qRound(event->globalPos().x()*devicePixelsPerPixel), qRound(event->globalPos().y()*devicePixelsPerPixel)),
			       event->delta(), event->buttons(), event->modifiers(), event->orientation());
#endif
	deltaEvent.setAccepted(false);
	// Send the event to every StelModule
	for (auto* i : moduleMgr->getCallOrders(StelModule::ActionHandleMouseClicks)) {
		i->handleMouseWheel(&deltaEvent);
		if (deltaEvent.isAccepted()) {
			event->accept();
			break;
		}
	}
}

// Handle mouse move
bool StelApp::handleMove(qreal x, qreal y, Qt::MouseButtons b)
{
	if (viewportEffect)
		viewportEffect->distortXY(x, y);
	// Send the event to every StelModule
	for (auto* i : moduleMgr->getCallOrders(StelModule::ActionHandleMouseMoves))
	{
		if (i->handleMouseMoves(qRound(x*devicePixelsPerPixel), qRound(y*devicePixelsPerPixel), b))
			return true;
	}
	return false;
}

// Handle key press and release
void StelApp::handleKeys(QKeyEvent* event)
{
	event->setAccepted(false);
	// First try to trigger a shortcut.
	if (event->type() == QEvent::KeyPress)
	{
		if (getStelActionManager()->pushKey(event->key() + int(event->modifiers())))
		{
			event->setAccepted(true);
			return;
		}
	}
	// Send the event to every StelModule
	for (auto* i : moduleMgr->getCallOrders(StelModule::ActionHandleKeys))
	{
		i->handleKeys(event);
		if (event->isAccepted())
			return;
	}
}

// Handle pinch on multi touch devices
void StelApp::handlePinch(qreal scale, bool started)
{
	// Send the event to every StelModule
	for (auto* i : moduleMgr->getCallOrders(StelModule::ActionHandleMouseMoves))
	{
		if (i->handlePinch(scale, started))
			return;
	}
}

//! Set flag for activating night vision mode
void StelApp::setVisionModeNight(bool b)
{
	if (flagNightVision!=b)
	{
		flagNightVision=b;
		emit visionNightModeChanged(b);
	}
}

void StelApp::setFlagOverwriteInfoColor(bool b)
{
	if (flagOverwriteInfoColor!=b)
	{
		flagOverwriteInfoColor=b;
		StelApp::immediateSave("gui/flag_overwrite_info_color", b);
		emit flagOverwriteInfoColorChanged(b);
	}
}

void StelApp::setFlagUseDecDegreesCoords(bool b)
{
	if (flagUseDecDegreesCoords!=b)
	{
		flagUseDecDegreesCoords = b;
		StelApp::immediateSave("gui/flag_use_decimal_degrees_coords", b);
		emit flagUseDecDegreesCoordsChanged(b);
	}
}

void StelApp::setFlagUseDecDegreesOther(bool b)
{
	if (flagUseDecDegreesOther!=b)
	{
		flagUseDecDegreesOther = b;
		StelApp::immediateSave("gui/flag_use_decimal_degrees_other", b);
		emit flagUseDecDegreesOtherChanged(b);
	}
}

void StelApp::setFlagSouthAzimuthUsage(bool use)
{
	if (flagUseAzimuthFromSouth!=use)
	{
		flagUseAzimuthFromSouth=use;
		StelApp::immediateSave("gui/flag_use_azimuth_from_south", use);
		emit flagUseAzimuthFromSouthChanged(use);
	}
}

void StelApp::setFlagUseNegativeHourAngles(bool use)
{
	if (flagUseNegativeHourAngles!=use)
	{
		flagUseNegativeHourAngles=use;
		StelApp::immediateSave("gui/flag_use_negative_hour_angles", use);
		emit flagUseNegativeHourAnglesChanged(use);
	}
}

void StelApp::setFlagPolarDistanceUsage(bool use)
{
	if (flagUsePolarDistance!=use)
	{
		flagUsePolarDistance=use;
		StelApp::immediateSave("gui/flag_use_polar_distance", use);
		emit flagUsePolarDistanceChanged(use);
	}
}


void StelApp::setFlagUseFormattingOutput(bool b)
{
	if (flagUseFormattingOutput!=b)
	{
		flagUseFormattingOutput = b;
		StelApp::immediateSave("gui/flag_use_formatting_output", flagUseFormattingOutput);
		emit flagUseFormattingOutputChanged(b);
	}
}

void StelApp::setFlagUseCCSDesignation(bool b)
{
	if (flagUseCCSDesignation!=b)
	{
		flagUseCCSDesignation = b;
		StelApp::immediateSave("gui/flag_use_ccs_designations", flagUseCCSDesignation);
		emit flagUseCCSDesignationChanged(b);
	}
}

void StelApp::setOverwriteInfoColor(const Vec3f& color)
{
	if (color != overwriteInfoColor)
	{
		overwriteInfoColor = color;
		emit overwriteInfoColorChanged(color);
	}
}

Vec3f StelApp::getOverwriteInfoColor() const
{
	return overwriteInfoColor;
}

void StelApp::setDaylightInfoColor(const Vec3f& color)
{
	if (color != daylightInfoColor)
	{
		daylightInfoColor = color;
		emit daylightInfoColorChanged(color);
	}
}

Vec3f StelApp::getDaylightInfoColor() const
{
	return daylightInfoColor;
}

void StelApp::setFlagImmediateSave(bool b)
{
	if (flagImmediateSave!=b)
	{
		flagImmediateSave = b;
		// To not accidentally activate this mode permanently, you must use the "save settings" button to activate!
		if (!b)
			StelApp::immediateSave("gui/immediate_save_details", false);
		emit flagImmediateSaveChanged(b);
	}
}

void StelApp::immediateSave(const QString &key, const QVariant &value)
{
	if (getInstance().getFlagImmediateSave())
		getInstance().getSettings()->setValue(key, value);
}


// Update translations and font for sky everywhere in the program
void StelApp::updateI18n()
{
#ifdef ENABLE_NLS
	emit languageChanged();
#endif
}

void StelApp::ensureGLContextCurrent()
{
	mainWin->glContextMakeCurrent();
}

// Return the time since when stellarium is running in second.
double StelApp::getTotalRunTime()
{
	return static_cast<double>(QDateTime::currentMSecsSinceEpoch() - StelApp::startMSecs)/1000.;
}

// Return the scaled time since when stellarium is running in second.
double StelApp::getAnimationTime()
{
	return static_cast<double>(QDateTime::currentMSecsSinceEpoch() - StelApp::startMSecs)*StelApp::animationScale/1000.;
}

void StelApp::reportFileDownloadFinished(QNetworkReply* reply)
{
	bool fromCache = reply->attribute(QNetworkRequest::SourceIsFromCacheAttribute).toBool();
	if (fromCache)
	{
		++nbUsedCache;
		totalUsedCacheSize+=reply->bytesAvailable();
	}
	else
	{
		++nbDownloadedFiles;
		totalDownloadedSize+=reply->bytesAvailable();
	}
}

void StelApp::quit()
{
	emit aboutToQuit();
	// Let's allow exit from Stellarium via startup script!
	QMetaObject::invokeMethod(qApp, "quit", Qt::QueuedConnection);
}

void StelApp::setDevicePixelsPerPixel(qreal dppp)
{
	// Check that the device-independent pixel size didn't change
	if (!fuzzyEquals(devicePixelsPerPixel, dppp))
	{
		qDebug() << "Changing high-DPI scaling factor from" << devicePixelsPerPixel << "to" << dppp;
		const auto effect = getViewportEffect();
		setViewportEffect("none");
		devicePixelsPerPixel = dppp;
		StelProjector::StelProjectorParams params = core->getCurrentStelProjectorParams();
		params.devicePixelsPerPixel = devicePixelsPerPixel;
		core->setCurrentStelProjectorParams(params);
		// Force to recreate the viewport effect if any.
		setViewportEffect(effect);
	}
}

void StelApp::setViewportEffect(const QString& name)
{
	if (name == getViewportEffect()) return;
	StelApp::immediateSave("video/viewport_effect", name);
	if (renderBuffer)
	{
		ensureGLContextCurrent();
		delete renderBuffer;
		renderBuffer = nullptr;
	}
	if (viewportEffect)
	{
		delete viewportEffect;
		viewportEffect = nullptr;
	}
	if (name == "none") return;

	if (!core)
	{
		qDebug() << "No core to set viewport effect";
		return;
	}
	StelProjector::StelProjectorParams params = core->getCurrentStelProjectorParams();
	int w = params.viewportXywh[2];
	int h = params.viewportXywh[3];
	if (name == "sphericMirrorDistorter")
	{
		viewportEffect = new StelViewportDistorterFisheyeToSphericMirror(w, h);
	}
	else
	{
		qDebug() << "unknown viewport effect name:" << name;
		Q_ASSERT(false);
	}
}

QString StelApp::getViewportEffect() const
{
	if (viewportEffect)
		return viewportEffect->getName();
	return "none";
}

// Diagnostics
void StelApp::dumpModuleActionPriorities(StelModule::StelModuleActionName actionName) const
{
	const QList<StelModule*> modules = moduleMgr->getCallOrders(actionName);
	QMetaEnum me = QMetaEnum::fromType<StelModule::StelModuleActionName>();
	qDebug() << "Module Priorities for action named" << me.valueToKey(actionName);

	for (auto* module : modules)
	{
		module->draw(core);
		qDebug() << " -- " << module->getCallOrder(actionName) << "Module: " << module->objectName();
	}
}

StelModule* StelApp::getModule(const QString& moduleID) const
{
	return getModuleMgr().getModule(moduleID);
}
void StelApp::setScreenFontSize(int s)
{
	if (screenFontSize!=s)
	{
		screenFontSize=s;
		StelApp::immediateSave("gui/screen_font_size", s);
		emit screenFontSizeChanged(s);
	}
}

void StelApp::setScreenButtonScale(const double s)
{
	if (screenButtonScale!=s)
	{
		screenButtonScale = s;
		StelApp::immediateSave("gui/screen_button_scale", s);
		emit screenButtonScaleChanged(s);
	}
}

double StelApp::screenFontSizeRatio() const
{
	return double(getScreenFontSize()) / getDefaultGuiFontSize();
}

void StelApp::setGuiFontSize(int s)
{
	if (getGuiFontSize()!=s)
	{
		QFont font=QGuiApplication::font();
		font.setPixelSize(s);
		QGuiApplication::setFont(font);
		StelApp::immediateSave("gui/gui_font_size", s);
		emit guiFontSizeChanged(s);
	}
}
int StelApp::getGuiFontSize() const
{
	return QGuiApplication::font().pixelSize();
}

float StelApp::getScreenScale() const
{
	const float dppRatio = StelApp::getInstance().getDevicePixelsPerPixel();
	const float fontRatio = StelApp::getInstance().screenFontSizeRatio();
	return dppRatio * fontRatio;
}

float StelApp::getGuiScale() const
{
	const float dppRatio = StelApp::getInstance().getDevicePixelsPerPixel();
	const float fontRatio = StelApp::getInstance().guiFontSizeRatio();
	return dppRatio * fontRatio;
}

void StelApp::setAppFont(QFont font)
{
	int oldSize=QGuiApplication::font().pixelSize();
	font.setPixelSize(oldSize);
	#if (QT_VERSION>=QT_VERSION_CHECK(5,15,0))
	font.setStyleHint(QFont::AnyStyle, QFont::PreferAntialias);
	#else
	font.setStyleHint(QFont::AnyStyle, QFont::OpenGLCompatible);
	#endif
	QGuiApplication::setFont(font);
	emit fontChanged(font);
}

QString StelApp::getVersion() const
{
	return StelUtils::getApplicationVersion();
}

void StelApp::enableBottomStelBarUpdates(bool enable)
{
#ifndef NO_GUI
	StelGui *gui=dynamic_cast<StelGui*>(getGui());
	gui->getButtonBar()->enableTopoCentricUpdate(enable);
#endif
}

void StelApp::dumpFontInfo() const
{
#if (QT_VERSION<QT_VERSION_CHECK(6,0,0))
	qInfo() << "StelApp::dumpFontInfo() not available for Qt5 builds.";
#else
	const QList<QFontDatabase::WritingSystem> writingSystems=QFontDatabase::writingSystems();
	qInfo() << "WritingSystems and Font Families:";
	for (const auto &ws: writingSystems)
	{
		qInfo() << ws << ":" << QFontDatabase::families(ws);
	}
#endif
}
