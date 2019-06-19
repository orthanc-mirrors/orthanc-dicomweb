var DICOM_TAG_ACCESSION_NUMBER = '00080050';
var DICOM_TAG_MODALITY = '00080060';
var DICOM_TAG_PATIENT_ID = '00100020';
var DICOM_TAG_PATIENT_NAME = '00100010';
var DICOM_TAG_SERIES_DESCRIPTION = '0008103E';
var DICOM_TAG_SERIES_INSTANCE_UID = '0020000E';
var DICOM_TAG_SOP_INSTANCE_UID = '00080018';
var DICOM_TAG_STUDY_DATE = '00080020';
var DICOM_TAG_STUDY_ID = '00200010';
var DICOM_TAG_STUDY_INSTANCE_UID = '0020000D';
var DEFAULT_PREVIEW = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAUAAAAFCAYAAACNbyblAAAAHElEQVQI12P4//8/w38GIAXDIBKE0DHxgljNBAAO9TXL0Y4OHwAAAABJRU5ErkJggg==';
var MAX_RESULTS = 100;

var app = new Vue({
  el: '#app',
  computed: {
    studiesCount() {
      return this.studies.length
    },
    seriesCount() {
      return this.series.length
    }
  },
  data: {
    orthancExplorerUri: '../../../',
    previewFailure: true,
    preview: DEFAULT_PREVIEW,
    showTruncatedStudies: false,
    showNoServer: false,
    showStudies: false,
    showSeries: false,
    maxResults: MAX_RESULTS,
    currentPage: 0,
    perPage: 10,
    servers: [ ],
    serversInfo: { },
    lookup: { },
    studies: [ ],
    currentStudy: null,
    studiesFields: [
      {
        key: DICOM_TAG_PATIENT_ID + '.Value',
        label: 'Patient ID',
        sortable: true
      },
      {
        key: DICOM_TAG_PATIENT_NAME + '.Value',
        label: 'Patient name',
        sortable: true
      },
      {
        key: DICOM_TAG_ACCESSION_NUMBER + '.Value',
        label: 'Accession number',
        sortable: true
      },
      {
        key: DICOM_TAG_STUDY_DATE + '.Value',
        label: 'Study date',
        sortable: true
      },
      {
        key: 'operations',
        label: ''
      }
    ],
    studyToDelete: null,
    studyTags: [ ],    
    studyTagsFields: [
      {
        key: 'Tag',
        sortable: true
      },
      {
        key: 'Name',
        label: 'Description',
        sortable: true
      },
      {
        key: 'Value',
        sortable: true
      }
    ],
    series: [ ],
    seriesFields: [
      {
        key: DICOM_TAG_SERIES_DESCRIPTION + '.Value',
        label: 'Series description',
        sortable: true
      },
      {
        key: DICOM_TAG_MODALITY + '.Value',
        label: 'Modality',
        sortable: true
      },
      {
        key: 'operations',
        label: ''
      }
    ],
    seriesToDelete: null,
    seriesTags: [ ],    
    seriesTagsFields: [
      {
        key: 'Tag',
        sortable: true
      },
      {
        key: 'Name',
        label: 'Description',
        sortable: true
      },
      {
        key: 'Value',
        sortable: true
      }
    ],
    scrollToSeries: false,
    scrollToStudies: false
  },
  mounted: () => {
    axios
      .get('../../servers?expand')
      .then(response => {
        app.serversInfo = response.data;
        app.servers = Object.keys(response.data).map(i => i);
        app.Clear();
      });
    axios
      .get('../info')
      .then(response => {
        app.orthancExplorerUri = response.data.OrthancRoot + '../../';
      });
  },
  methods: {
    /**
     * Toolbox
     **/

    ScrollToRef: function(refName) {
      var element = app.$refs[refName];
      window.scrollTo(0, element.offsetTop + 200);
    },
    ShowErrorModal: function() {
      app.$refs['modal-error'].show();
    },

    
    /**
     * Studies
     **/

    SetStudies: function(response) {
      if (response.data.length > app.maxResults) {
        app.showTruncatedStudies = true;
        app.studies = response.data.splice(0, app.maxResults);
      } else {
        app.showTruncatedStudies = false;
        app.studies = response.data;
      }
      app.showStudies = true;
      app.showSeries = false;
      app.studyToDelete = null;
      app.scrollToStudies = true;
    },
    ExecuteLookup: function() {
      var args = { 
        'fuzzymatching' : 'true',
        'limit' : (app.maxResults + 1).toString()
      };

      if ('patientName' in app.lookup) {
        args[DICOM_TAG_PATIENT_NAME] = app.lookup.patientName;
      }

      if ('patientID' in app.lookup) {
        args[DICOM_TAG_PATIENT_ID] = app.lookup.patientID;
      }

      if ('studyDate' in app.lookup) {
        args[DICOM_TAG_STUDY_DATE] = app.lookup.studyDate;
      }

      if ('accessionNumber' in app.lookup) {
        args[DICOM_TAG_ACCESSION_NUMBER] = app.lookup.accessionNumber;
      }
      
      axios
        .post('../../servers/' + app.lookup.server + '/qido', {
          'Uri' : '/studies',
          'Arguments' : args,
        })
        .then(app.SetStudies)
        .catch(app.ShowErrorModal)
    },
    Clear: function() {
      app.lookup = {};
      currentStudy = null;
      app.showSeries = false;
      app.showStudies = false;
      if (app.servers.length == 0) {
        app.showNoServer = true;
      } else {
        app.showNoServer = false;
        app.lookup.server = app.servers[0];
      }
    },
    OnLookup: function(event) {
      event.preventDefault();
      app.ExecuteLookup();
    },
    OnReset: function(event) {
      event.preventDefault();
      app.Clear();
    },
    OpenStudyDetails: function(study) {
      app.studyTags = Object.keys(study).map(i => {
        var item = study[i];
        item['Tag'] = i;
        return item;
      });
      
      app.$refs['study-details'].show();
    },
    ConfirmDeleteStudy: function(study) {
      app.studyToDelete = study;
      app.$bvModal.show('study-delete-confirm');
    },
    ExecuteDeleteStudy: function(study) {
      axios
        .post('../../servers/' + app.lookup.server + '/delete', {
          'Level': 'Study',
          'StudyInstanceUID': app.studyToDelete[DICOM_TAG_STUDY_INSTANCE_UID].Value
        })
        .then(app.ExecuteLookup)
        .catch(app.ShowErrorModal)
    },

    
    /**
     * Series
     **/

    LoadSeriesOfCurrentStudy: function() {
      axios
        .post('../../servers/' + app.lookup.server + '/qido', {
          'Uri' : '/studies/' + app.currentStudy + '/series'
        })
        .then(response => {
          app.series = response.data;
          app.showSeries = true;
          app.seriesToDelete = null;
          app.scrollToSeries = true;
        })
        .catch(app.ExecuteLookup);   // Parent study was deleted
    }, 
    OpenSeries: function(series) {
      app.currentStudy = series[DICOM_TAG_STUDY_INSTANCE_UID].Value;
      app.LoadSeriesOfCurrentStudy();
    },
    OpenSeriesDetails: function(series) {
      app.seriesTags = Object.keys(series).map(i => {
        var item = series[i];
        item['Tag'] = i;
        return item;
      });
      
      app.$refs['series-details'].show();
    },
    OpenSeriesPreview: function(series) {
      axios
        .post('../../servers/' + app.lookup.server + '/get', {
          'Uri' : ('/studies/' + app.currentStudy + '/series/' + 
                   series[DICOM_TAG_SERIES_INSTANCE_UID].Value + '/instances')
        })
        .then(response => {
          var instance = response.data[Math.floor(response.data.length / 2)];

          axios
            .post('../../servers/' + app.lookup.server + '/get', {
              'Uri' : ('/studies/' + app.currentStudy + '/series/' + 
                       series[DICOM_TAG_SERIES_INSTANCE_UID].Value + '/instances/' +
                       instance[DICOM_TAG_SOP_INSTANCE_UID].Value + '/rendered')
            }, {
              responseType: 'arraybuffer'
            })
            .then(response => {
              // https://github.com/axios/axios/issues/513
              var image = btoa(new Uint8Array(response.data)
                               .reduce((data, byte) => data + String.fromCharCode(byte), ''));
              app.preview = ("data:" + 
                             response.headers['content-type'].toLowerCase() + 
                             ";base64," + image);
              app.previewFailure = false;
            })
            .catch(response => {
              app.previewFailure = true;
            })
              .finally(function() {
                app.$refs['series-preview'].show();
              })
        })
    },
    ConfirmDeleteSeries: function(series) {
      app.seriesToDelete = series;
      app.$bvModal.show('series-delete-confirm');
    },
    ExecuteDeleteSeries: function(series) {
      axios
        .post('../../servers/' + app.lookup.server + '/delete', {
          'Level': 'Series',
          'StudyInstanceUID': app.currentStudy,
          'SeriesInstanceUID': app.seriesToDelete[DICOM_TAG_SERIES_INSTANCE_UID].Value
        })
        .then(app.LoadSeriesOfCurrentStudy)
        .catch(app.ShowErrorModal)
    }
  },

  updated: function () {
    this.$nextTick(function () {
      // Code that will run only after the
      // entire view has been re-rendered

      if (app.scrollToStudies) {
        app.scrollToStudies = false;
        app.ScrollToRef('studies-top');
      }

      if (app.scrollToSeries) {
        app.scrollToSeries = false;
        app.ScrollToRef('series-top');
      }
    })
  }
});
