This is a sample JavaScript/jQuery application that makes a QIDO-RS request.

To use it without facing the cross-domain restrictions, you will need
to serve this folder using the "ServeFolder" plugin from the Orthanc
source distribution:
https://code.google.com/p/orthanc/source/browse/#hg%2FPlugins%2FSamples%2FServeFolders

Your Orthanc configuration file will have to contain the following
instruction to serve this folder as "http://localhost:8042/test/":

  "ServeFolders" : {
    "/test" : "../Samples/JavaScript"
  }

Once Orthanc is running with the adapted configuration, open Firefox
or Chrome at the following URL:
http://localhost:8042/test/index.html
